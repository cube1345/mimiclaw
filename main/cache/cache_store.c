#include "cache/cache_store.h"

#include "mimi_config.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "cache";

typedef struct {
    bool used;
    char key[MIMI_CACHE_MAX_KEY_BYTES];
    char *value;
    size_t value_len;
    int64_t expires_at_us;
    int64_t last_access_us;
    uint32_t hits;
} cache_entry_t;

static cache_entry_t s_entries[MIMI_CACHE_MAX_ENTRIES];
static SemaphoreHandle_t s_lock;
static uint32_t s_hits;
static uint32_t s_misses;
static uint32_t s_evictions;
static uint32_t s_expired;
static size_t s_total_bytes;

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

static bool is_expired(const cache_entry_t *entry, int64_t now)
{
    return entry->used && entry->expires_at_us > 0 && now >= entry->expires_at_us;
}

static void free_entry(cache_entry_t *entry)
{
    if (!entry || !entry->used) {
        return;
    }

    s_total_bytes -= entry->value_len;
    free(entry->value);
    memset(entry, 0, sizeof(*entry));
}

static char *alloc_value(size_t len)
{
    char *ptr = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr) {
        ptr = malloc(len);
    }
    return ptr;
}

static int find_entry(const char *key)
{
    for (int i = 0; i < MIMI_CACHE_MAX_ENTRIES; ++i) {
        if (s_entries[i].used && strcmp(s_entries[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_free_entry(void)
{
    for (int i = 0; i < MIMI_CACHE_MAX_ENTRIES; ++i) {
        if (!s_entries[i].used) {
            return i;
        }
    }
    return -1;
}

static int find_victim(int64_t now)
{
    int victim = -1;
    int64_t oldest_access = INT64_MAX;

    for (int i = 0; i < MIMI_CACHE_MAX_ENTRIES; ++i) {
        if (!s_entries[i].used) {
            return i;
        }
        if (is_expired(&s_entries[i], now)) {
            s_expired++;
            return i;
        }
        if (s_entries[i].last_access_us < oldest_access) {
            oldest_access = s_entries[i].last_access_us;
            victim = i;
        }
    }

    return victim;
}

static int find_used_victim(int64_t now)
{
    int victim = -1;
    int64_t oldest_access = INT64_MAX;

    for (int i = 0; i < MIMI_CACHE_MAX_ENTRIES; ++i) {
        if (!s_entries[i].used) {
            continue;
        }
        if (is_expired(&s_entries[i], now)) {
            s_expired++;
            return i;
        }
        if (s_entries[i].last_access_us < oldest_access) {
            oldest_access = s_entries[i].last_access_us;
            victim = i;
        }
    }

    return victim;
}

static void evict_until_fits(size_t incoming_len, int protected_idx, int64_t now)
{
    while (s_total_bytes + incoming_len > MIMI_CACHE_MAX_TOTAL_BYTES) {
        int victim = find_used_victim(now);
        if (victim < 0 || victim == protected_idx) {
            break;
        }
        free_entry(&s_entries[victim]);
        s_evictions++;
    }
}

esp_err_t cache_store_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "RAM KV cache ready: entries=%d total=%d value=%d",
             MIMI_CACHE_MAX_ENTRIES,
             MIMI_CACHE_MAX_TOTAL_BYTES,
             MIMI_CACHE_MAX_VALUE_BYTES);
    return ESP_OK;
}

esp_err_t cache_get(const char *key, char *out, size_t out_size)
{
    if (!key || !key[0] || !out || out_size == 0 || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }

    out[0] = '\0';

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = find_entry(key);
    int64_t now = now_us();
    if (idx < 0) {
        s_misses++;
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    cache_entry_t *entry = &s_entries[idx];
    if (is_expired(entry, now)) {
        free_entry(entry);
        s_misses++;
        s_expired++;
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    size_t copy = entry->value_len < out_size - 1 ? entry->value_len : out_size - 1;
    memcpy(out, entry->value, copy);
    out[copy] = '\0';
    entry->last_access_us = now;
    entry->hits++;
    s_hits++;

    xSemaphoreGive(s_lock);
    return (copy == entry->value_len) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t cache_put(const char *key, const char *value, uint32_t ttl_s)
{
    if (!key || !key[0] || !value || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t key_len = strlen(key);
    size_t value_len = strlen(value);
    if (key_len >= MIMI_CACHE_MAX_KEY_BYTES || value_len > MIMI_CACHE_MAX_VALUE_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *copy = alloc_value(value_len + 1);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, value, value_len + 1);

    int64_t now = now_us();
    int64_t expires_at = now + ((int64_t)(ttl_s ? ttl_s : MIMI_CACHE_DEFAULT_TTL_S) * 1000000LL);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = find_entry(key);
    if (idx >= 0) {
        free_entry(&s_entries[idx]);
    }

    evict_until_fits(value_len, idx, now);
    if (idx < 0) {
        idx = find_free_entry();
        if (idx < 0) {
            idx = find_victim(now);
            if (idx >= 0) {
                free_entry(&s_entries[idx]);
                s_evictions++;
            }
        }
    }

    if (idx < 0 || s_total_bytes + value_len > MIMI_CACHE_MAX_TOTAL_BYTES) {
        xSemaphoreGive(s_lock);
        free(copy);
        return ESP_ERR_NO_MEM;
    }

    cache_entry_t *entry = &s_entries[idx];
    memset(entry, 0, sizeof(*entry));
    entry->used = true;
    memcpy(entry->key, key, key_len + 1);
    entry->value = copy;
    entry->value_len = value_len;
    entry->expires_at_us = expires_at;
    entry->last_access_us = now;
    s_total_bytes += value_len;

    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t cache_delete(const char *key)
{
    if (!key || !key[0] || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = find_entry(key);
    if (idx < 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    free_entry(&s_entries[idx]);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t cache_delete_prefix(const char *prefix)
{
    if (!prefix || !s_lock) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t prefix_len = strlen(prefix);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MIMI_CACHE_MAX_ENTRIES; ++i) {
        if (s_entries[i].used && strncmp(s_entries[i].key, prefix, prefix_len) == 0) {
            free_entry(&s_entries[i]);
        }
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void cache_stats(cache_stats_t *stats)
{
    if (!stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));
    if (!s_lock) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    stats->hits = s_hits;
    stats->misses = s_misses;
    stats->evictions = s_evictions;
    stats->expired = s_expired;
    stats->bytes = (uint32_t)s_total_bytes;
    for (int i = 0; i < MIMI_CACHE_MAX_ENTRIES; ++i) {
        if (s_entries[i].used) {
            stats->entries++;
        }
    }
    xSemaphoreGive(s_lock);
}

void cache_clear(void)
{
    if (!s_lock) {
        return;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MIMI_CACHE_MAX_ENTRIES; ++i) {
        free_entry(&s_entries[i]);
    }
    s_total_bytes = 0;
    xSemaphoreGive(s_lock);
}
