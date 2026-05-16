#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"

#include "mimi_config.h"
#include "bus/message_bus.h"
#include "wifi/wifi_manager.h"
#include "channels/telegram/telegram_bot.h"
#include "channels/feishu/feishu_bot.h"
#include "llm/llm_proxy.h"
#include "agent/agent_loop.h"
#include "cache/cache_store.h"
#include "memory/memory_store.h"
#include "memory/session_mgr.h"
#include "gateway/ws_server.h"
#include "cli/serial_cli.h"
#include "proxy/http_proxy.h"
#include "tools/tool_registry.h"
#include "tools/tool_sgp30.h"
#include "tools/tool_aht10.h"
#include "tools/tool_hc_sr05.h"
#include "tools/tool_servo.h"
#include "cron/cron_service.h"
#include "heartbeat/heartbeat.h"
#include "skills/skill_loader.h"
#include "onboard/wifi_onboard.h"
#include "sensors/sensor_mqtt.h"

static const char *TAG = "mimi";

#define MIMI_PRESENCE_MONITOR_INTERVAL_MS 1000
#define MIMI_AHT10_MONITOR_INTERVAL_MS 2000

static void boot_servo_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(1500));

    ESP_LOGI(TAG, "Boot servo demo: moving servo on GPIO%d", MIMI_SERVO_DEFAULT_GPIO);
    if (tool_servo_set_angle(90) != ESP_OK) {
        ESP_LOGW(TAG, "Boot servo demo failed at 90 degrees");
        vTaskDelete(NULL);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(600));

    if (tool_servo_set_angle(0) != ESP_OK) {
        ESP_LOGW(TAG, "Boot servo demo failed at 0 degrees");
    }

    ESP_LOGI(TAG, "Boot servo demo complete");
    vTaskDelete(NULL);
}

static void aht10_monitor_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(2000));

    ESP_LOGI(TAG,
             "AHT10 monitor started: SDA=GPIO%d SCL=GPIO%d addr=0x%02x interval=%dms, bypassing LLM",
             MIMI_AHT10_DEFAULT_SDA_GPIO,
             MIMI_AHT10_DEFAULT_SCL_GPIO,
             MIMI_AHT10_DEFAULT_ADDR,
             MIMI_AHT10_MONITOR_INTERVAL_MS);

    while (1) {
        char output[256] = {0};
        esp_err_t err = tool_aht10_read_temperature_humidity_execute("{}", output, sizeof(output));
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "AHT10 monitor: %s", output);
        } else {
            ESP_LOGW(TAG, "AHT10 monitor failed: %s (%s)", esp_err_to_name(err), output);
        }

        vTaskDelay(pdMS_TO_TICKS(MIMI_AHT10_MONITOR_INTERVAL_MS));
    }
}

static void presence_monitor_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Presence monitor started on GPIO%d, interval=%dms",
             MIMI_PRESENCE_DEFAULT_GPIO, MIMI_PRESENCE_MONITOR_INTERVAL_MS);

    while (1) {
        char output[256] = {0};
        esp_err_t err = tool_read_presence_execute("{}", output, sizeof(output));
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Presence monitor: %s", output);
        } else {
            ESP_LOGW(TAG, "Presence monitor failed: %s (%s)", esp_err_to_name(err), output);
        }
        vTaskDelay(pdMS_TO_TICKS(MIMI_PRESENCE_MONITOR_INTERVAL_MS));
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static esp_err_t init_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = MIMI_SPIFFS_BASE,
        .partition_label = NULL,
        .max_files = 10,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);
    ESP_LOGI(TAG, "SPIFFS: total=%d, used=%d", (int)total, (int)used);

    return ESP_OK;
}

/* Outbound dispatch task: reads from outbound queue and routes to channels */
static void outbound_dispatch_task(void *arg)
{
    ESP_LOGI(TAG, "Outbound dispatch started");

    while (1) {
        mimi_msg_t msg;
        if (message_bus_pop_outbound(&msg, UINT32_MAX) != ESP_OK) continue;

        ESP_LOGI(TAG, "Dispatching response to %s:%s", msg.channel, msg.chat_id);

        if (strcmp(msg.channel, MIMI_CHAN_TELEGRAM) == 0) {
            esp_err_t send_err = telegram_send_message(msg.chat_id, msg.content);
            if (send_err != ESP_OK) {
                ESP_LOGE(TAG, "Telegram send failed for %s: %s", msg.chat_id, esp_err_to_name(send_err));
            } else {
                ESP_LOGI(TAG, "Telegram send success for %s (%d bytes)", msg.chat_id, (int)strlen(msg.content));
            }
        } else if (strcmp(msg.channel, MIMI_CHAN_FEISHU) == 0) {
            esp_err_t send_err = feishu_send_message(msg.chat_id, msg.content);
            if (send_err != ESP_OK) {
                ESP_LOGE(TAG, "Feishu send failed for %s: %s", msg.chat_id, esp_err_to_name(send_err));
            } else {
                ESP_LOGI(TAG, "Feishu send success for %s (%d bytes)", msg.chat_id, (int)strlen(msg.content));
            }
        } else if (strcmp(msg.channel, MIMI_CHAN_WEBSOCKET) == 0) {
            esp_err_t ws_err = ws_server_send(msg.chat_id, msg.content);
            if (ws_err != ESP_OK) {
                ESP_LOGW(TAG, "WS send failed for %s: %s", msg.chat_id, esp_err_to_name(ws_err));
            }
        } else if (strcmp(msg.channel, MIMI_CHAN_SYSTEM) == 0) {
            ESP_LOGI(TAG, "System message [%s]: %.128s", msg.chat_id, msg.content);
        } else {
            ESP_LOGW(TAG, "Unknown channel: %s", msg.channel);
        }

        free(msg.content);
    }
}

void app_main(void)
{
    /* Silence noisy components */
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  MimiClaw - ESP32-S3 AI Agent");
    ESP_LOGI(TAG, "========================================");

    /* Print memory info */
    ESP_LOGI(TAG, "Internal free: %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(TAG, "PSRAM free:    %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* Phase 1: Core infrastructure */
    ESP_ERROR_CHECK(init_nvs());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(init_spiffs());

    /* Initialize subsystems */
    ESP_ERROR_CHECK(message_bus_init());
    ESP_ERROR_CHECK(memory_store_init());
    ESP_ERROR_CHECK(cache_store_init());
    ESP_ERROR_CHECK(skill_loader_init());
    ESP_ERROR_CHECK(session_mgr_init());
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(http_proxy_init());
    ESP_ERROR_CHECK(telegram_bot_init());
    ESP_ERROR_CHECK(feishu_bot_init());
    ESP_ERROR_CHECK(llm_proxy_init());
    ESP_ERROR_CHECK(tool_registry_init());
    ESP_ERROR_CHECK(cron_service_init());
    ESP_ERROR_CHECK(heartbeat_init());
    ESP_ERROR_CHECK(agent_loop_init());

    /* Start Serial CLI first (works without WiFi) */
    ESP_ERROR_CHECK(serial_cli_init());

    ESP_ERROR_CHECK((xTaskCreatePinnedToCore(
        boot_servo_task, "boot_servo",
        3072, NULL, 4, NULL, 0) == pdPASS)
        ? ESP_OK : ESP_FAIL);

    ESP_ERROR_CHECK((xTaskCreatePinnedToCore(
        aht10_monitor_task, "aht10_mon",
        4096, NULL, 4, NULL, 0) == pdPASS)
        ? ESP_OK : ESP_FAIL);

    ESP_ERROR_CHECK((xTaskCreatePinnedToCore(
        presence_monitor_task, "presence_mon",
        3072, NULL, 4, NULL, 0) == pdPASS)
        ? ESP_OK : ESP_FAIL);

    if (tool_sgp30_monitor_start() != ESP_OK) {
        ESP_LOGW(TAG, "SGP30 auto monitor unavailable");
    }

    /* Start WiFi */
    esp_err_t wifi_err = wifi_manager_start();
    bool wifi_ok = false;
    if (wifi_err == ESP_OK) {
        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        if (wifi_manager_wait_connected(30000) == ESP_OK) {
            wifi_ok = true;
            ESP_LOGI(TAG, "WiFi connected: %s", wifi_manager_get_ip());
        } else {
            ESP_LOGW(TAG, "WiFi connection timeout");
        }
    } else {
        ESP_LOGW(TAG, "No WiFi credentials configured");
    }

    if (!wifi_ok) {
        ESP_LOGW(TAG, "Entering WiFi onboarding mode...");
        wifi_onboard_start(WIFI_ONBOARD_MODE_CAPTIVE);  /* blocks, restarts on success */
        return;  /* unreachable */
    }

    if (wifi_onboard_start(WIFI_ONBOARD_MODE_ADMIN) != ESP_OK) {
        ESP_LOGW(TAG, "Local admin portal unavailable; continuing without config hotspot");
    }

    {
        /* Outbound dispatch task should start first to avoid dropping early replies. */
        ESP_ERROR_CHECK((xTaskCreatePinnedToCore(
            outbound_dispatch_task, "outbound",
            MIMI_OUTBOUND_STACK, NULL,
            MIMI_OUTBOUND_PRIO, NULL, MIMI_OUTBOUND_CORE) == pdPASS)
            ? ESP_OK : ESP_FAIL);

        /* Start network-dependent services */
        ESP_ERROR_CHECK(agent_loop_start());
        ESP_ERROR_CHECK(telegram_bot_start());
        ESP_ERROR_CHECK(feishu_bot_start());
        ESP_ERROR_CHECK(sensor_mqtt_start());
        cron_service_start();
        heartbeat_start();
        ESP_ERROR_CHECK(ws_server_start());

        ESP_LOGI(TAG, "All services started!");
    }

    ESP_LOGI(TAG, "MimiClaw ready. Type 'help' for CLI commands.");
}
