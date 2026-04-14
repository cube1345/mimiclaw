#include "tools/tool_gpio.h"
<<<<<<< HEAD
#include "mimi_config.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tool_gpio";

#define WS2812_RESOLUTION_HZ 10000000
#define WS2812_RESET_US      50

static const rmt_symbol_word_t s_ws2812_zero = {
    .level0 = 1,
    .duration0 = 3,
    .level1 = 0,
    .duration1 = 9,
};

static const rmt_symbol_word_t s_ws2812_one = {
    .level0 = 1,
    .duration0 = 9,
    .level1 = 0,
    .duration1 = 3,
};

static const rmt_symbol_word_t s_ws2812_reset = {
    .level0 = 0,
    .duration0 = (WS2812_RESOLUTION_HZ / 1000000 * WS2812_RESET_US) / 2,
    .level1 = 0,
    .duration1 = (WS2812_RESOLUTION_HZ / 1000000 * WS2812_RESET_US) / 2,
};

typedef struct {
    rmt_channel_handle_t channel;
    rmt_encoder_handle_t encoder;
    int gpio_num;
} ws2812_state_t;

static ws2812_state_t s_ws2812 = {
    .channel = NULL,
    .encoder = NULL,
    .gpio_num = -1,
};

static bool get_required_int(cJSON *root, const char *key, int *value)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!item || !cJSON_IsNumber(item)) {
        return false;
    }

    *value = item->valueint;
    return true;
}

static bool get_optional_int(cJSON *root, const char *key, int *value)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!item || !cJSON_IsNumber(item)) {
        return false;
    }

    *value = item->valueint;
    return true;
}

static esp_err_t ensure_output_gpio(int pin)
{
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    return gpio_config(&cfg);
}

static uint8_t clamp_u8(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

static uint8_t scale_component(uint8_t value, uint8_t brightness)
{
    return (uint8_t)(((uint16_t)value * brightness + 127) / 255);
}

static size_t ws2812_encoder_callback(const void *data, size_t data_size,
                                      size_t symbols_written, size_t symbols_free,
                                      rmt_symbol_word_t *symbols, bool *done, void *arg)
{
    (void)arg;

    if (symbols_free < 8) {
        return 0;
    }

    size_t data_pos = symbols_written / 8;
    const uint8_t *bytes = (const uint8_t *)data;
    if (data_pos < data_size) {
        size_t symbol_pos = 0;
        for (int bitmask = 0x80; bitmask != 0; bitmask >>= 1) {
            symbols[symbol_pos++] = (bytes[data_pos] & bitmask) ? s_ws2812_one : s_ws2812_zero;
        }
        return symbol_pos;
    }

    symbols[0] = s_ws2812_reset;
    *done = true;
    return 1;
}

static void ws2812_release(void)
{
    if (s_ws2812.channel) {
        rmt_disable(s_ws2812.channel);
    }
    if (s_ws2812.encoder) {
        rmt_del_encoder(s_ws2812.encoder);
        s_ws2812.encoder = NULL;
    }
    if (s_ws2812.channel) {
        rmt_del_channel(s_ws2812.channel);
        s_ws2812.channel = NULL;
    }
    s_ws2812.gpio_num = -1;
}

static esp_err_t ws2812_ensure_ready(int pin)
{
    esp_err_t err;

    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ws2812.channel && s_ws2812.encoder && s_ws2812.gpio_num == pin) {
        return ESP_OK;
    }

    if (s_ws2812.channel || s_ws2812.encoder) {
        ws2812_release();
    }

    rmt_tx_channel_config_t tx_chan_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = pin,
        .mem_block_symbols = 64,
        .resolution_hz = WS2812_RESOLUTION_HZ,
        .trans_queue_depth = 1,
    };
    err = rmt_new_tx_channel(&tx_chan_cfg, &s_ws2812.channel);
    if (err != ESP_OK) {
        ws2812_release();
        return err;
    }

    rmt_simple_encoder_config_t encoder_cfg = {
        .callback = ws2812_encoder_callback,
        .min_chunk_size = 8,
    };
    err = rmt_new_simple_encoder(&encoder_cfg, &s_ws2812.encoder);
    if (err != ESP_OK) {
        ws2812_release();
        return err;
    }

    err = rmt_enable(s_ws2812.channel);
    if (err != ESP_OK) {
        ws2812_release();
        return err;
    }

    s_ws2812.gpio_num = pin;
=======
#include "tools/gpio_policy.h"
#include "mimi_config.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "tool_gpio";

esp_err_t tool_gpio_init(void)
{
    ESP_LOGI(TAG, "GPIO tool initialized (pin range %d-%d)",
             MIMI_GPIO_MIN_PIN, MIMI_GPIO_MAX_PIN);
>>>>>>> bb10ea0149080d506d920c09054f4c5b20409de2
    return ESP_OK;
}

esp_err_t tool_gpio_write_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

<<<<<<< HEAD
    int pin = -1;
    int level = -1;
    if (!get_required_int(root, "pin", &pin) || !get_required_int(root, "level", &level)) {
        snprintf(output, output_size, "Error: missing required numeric fields 'pin' and 'level'");
=======
    cJSON *pin_obj = cJSON_GetObjectItem(root, "pin");
    cJSON *state_obj = cJSON_GetObjectItem(root, "state");

    if (!cJSON_IsNumber(pin_obj)) {
        snprintf(output, output_size, "Error: 'pin' required (integer)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    if (!cJSON_IsNumber(state_obj)) {
        snprintf(output, output_size, "Error: 'state' required (0 or 1)");
>>>>>>> bb10ea0149080d506d920c09054f4c5b20409de2
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

<<<<<<< HEAD
    if (level != 0 && level != 1) {
        snprintf(output, output_size, "Error: level must be 0 or 1");
=======
    int pin = (int)pin_obj->valuedouble;
    int state = (int)state_obj->valuedouble;

    if (!gpio_policy_pin_is_allowed(pin)) {
        if (gpio_policy_pin_forbidden_hint(pin, output, output_size)) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        if (MIMI_GPIO_ALLOWED_CSV[0] != '\0') {
            snprintf(output, output_size, "Error: pin %d is not in allowed list", pin);
        } else {
            snprintf(output, output_size, "Error: pin must be %d-%d",
                     MIMI_GPIO_MIN_PIN, MIMI_GPIO_MAX_PIN);
        }
>>>>>>> bb10ea0149080d506d920c09054f4c5b20409de2
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

<<<<<<< HEAD
    esp_err_t err = ensure_output_gpio(pin);
    if (err == ESP_OK) {
        err = gpio_set_level(pin, level);
    }

    if (err == ESP_OK) {
        snprintf(output, output_size, "OK: GPIO %d set %s", pin, level ? "HIGH" : "LOW");
    } else if (err == ESP_ERR_INVALID_ARG) {
        snprintf(output, output_size, "Error: GPIO %d is not a valid output pin", pin);
    } else {
        snprintf(output, output_size, "Error: failed to set GPIO %d (%s)", pin, esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "gpio_write pin=%d level=%d -> %s", pin, level, esp_err_to_name(err));
    cJSON_Delete(root);
    return err;
}

esp_err_t tool_ws2812_set_execute(const char *input_json, char *output, size_t output_size)
=======
    if (gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT) != ESP_OK ||
        gpio_set_level(pin, state ? 1 : 0) != ESP_OK) {
        snprintf(output, output_size, "Error: failed to configure/write pin %d", pin);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    snprintf(output, output_size, "Pin %d set to %s", pin, state ? "HIGH" : "LOW");
    ESP_LOGI(TAG, "gpio_write: pin %d -> %s", pin, state ? "HIGH" : "LOW");

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t tool_gpio_read_execute(const char *input_json, char *output, size_t output_size)
>>>>>>> bb10ea0149080d506d920c09054f4c5b20409de2
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

<<<<<<< HEAD
    int red = -1;
    int green = -1;
    int blue = -1;
    if (!get_required_int(root, "r", &red) ||
        !get_required_int(root, "g", &green) ||
        !get_required_int(root, "b", &blue)) {
        snprintf(output, output_size, "Error: missing required numeric fields 'r', 'g', and 'b'");
=======
    cJSON *pin_obj = cJSON_GetObjectItem(root, "pin");
    if (!cJSON_IsNumber(pin_obj)) {
        snprintf(output, output_size, "Error: 'pin' required (integer)");
>>>>>>> bb10ea0149080d506d920c09054f4c5b20409de2
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

<<<<<<< HEAD
    int pin = MIMI_WS2812_DEFAULT_GPIO;
    int brightness = 255;
    (void)get_optional_int(root, "pin", &pin);
    (void)get_optional_int(root, "brightness", &brightness);

    if (brightness < 0 || brightness > 255) {
        snprintf(output, output_size, "Error: brightness must be between 0 and 255");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ws2812_ensure_ready(pin);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_ARG) {
            snprintf(output, output_size, "Error: GPIO %d is not a valid WS2812 output pin", pin);
        } else {
            snprintf(output, output_size, "Error: failed to initialize WS2812 on GPIO %d (%s)", pin, esp_err_to_name(err));
        }
        cJSON_Delete(root);
        return err;
    }

    uint8_t rgb[3] = {
        scale_component(clamp_u8(green), (uint8_t)brightness),
        scale_component(clamp_u8(red), (uint8_t)brightness),
        scale_component(clamp_u8(blue), (uint8_t)brightness),
    };

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
    };
    err = rmt_transmit(s_ws2812.channel, s_ws2812.encoder, rgb, sizeof(rgb), &tx_cfg);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(s_ws2812.channel, 100);
    }

    if (err == ESP_OK) {
        snprintf(output, output_size,
                 "OK: WS2812 on GPIO %d set to RGB(%u,%u,%u) brightness=%d",
                 pin, (unsigned)clamp_u8(red), (unsigned)clamp_u8(green), (unsigned)clamp_u8(blue), brightness);
    } else {
        snprintf(output, output_size, "Error: failed to update WS2812 on GPIO %d (%s)", pin, esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "ws2812_set pin=%d rgb=(%d,%d,%d) brightness=%d -> %s",
             pin, red, green, blue, brightness, esp_err_to_name(err));
    cJSON_Delete(root);
    return err;
=======
    int pin = (int)pin_obj->valuedouble;

    if (!gpio_policy_pin_is_allowed(pin)) {
        if (gpio_policy_pin_forbidden_hint(pin, output, output_size)) {
            cJSON_Delete(root);
            return ESP_ERR_INVALID_ARG;
        }
        if (MIMI_GPIO_ALLOWED_CSV[0] != '\0') {
            snprintf(output, output_size, "Error: pin %d is not in allowed list", pin);
        } else {
            snprintf(output, output_size, "Error: pin must be %d-%d",
                     MIMI_GPIO_MIN_PIN, MIMI_GPIO_MAX_PIN);
        }
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    /* Enable input path, then read level */
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    int level = gpio_get_level(pin);

    snprintf(output, output_size, "Pin %d = %s", pin, level ? "HIGH" : "LOW");
    ESP_LOGI(TAG, "gpio_read: pin %d = %s", pin, level ? "HIGH" : "LOW");

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t tool_gpio_read_all_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

    char *cursor = output;
    size_t remaining = output_size;
    int written;
    int count = 0;

    written = snprintf(cursor, remaining, "GPIO states: ");
    if (written < 0 || (size_t)written >= remaining) {
        output[0] = '\0';
        return ESP_FAIL;
    }
    cursor += (size_t)written;
    remaining -= (size_t)written;

    if (MIMI_GPIO_ALLOWED_CSV[0] != '\0') {
        /* Iterate over explicit allowlist */
        const char *csv_cursor = MIMI_GPIO_ALLOWED_CSV;
        while (*csv_cursor != '\0') {
            char *endptr = NULL;
            long value;

            while (*csv_cursor == ' ' || *csv_cursor == '\t' || *csv_cursor == ',') {
                csv_cursor++;
            }
            if (*csv_cursor == '\0') break;

            value = strtol(csv_cursor, &endptr, 10);
            if (endptr == csv_cursor) {
                while (*csv_cursor != '\0' && *csv_cursor != ',') csv_cursor++;
                continue;
            }
            if (!gpio_policy_pin_is_allowed((int)value)) {
                csv_cursor = endptr;
                continue;
            }

            gpio_set_direction((int)value, GPIO_MODE_INPUT);
            int level = gpio_get_level((int)value);

            written = snprintf(cursor, remaining, "%s%d=%s",
                               count == 0 ? "" : ", ",
                               (int)value, level ? "HIGH" : "LOW");
            if (written < 0 || (size_t)written >= remaining) break;
            cursor += (size_t)written;
            remaining -= (size_t)written;
            count++;
            csv_cursor = endptr;
        }
    } else {
        /* Iterate over default range */
        for (int pin = MIMI_GPIO_MIN_PIN; pin <= MIMI_GPIO_MAX_PIN; pin++) {
            if (!gpio_policy_pin_is_allowed(pin)) continue;

            gpio_set_direction(pin, GPIO_MODE_INPUT);
            int level = gpio_get_level(pin);

            written = snprintf(cursor, remaining, "%s%d=%s",
                               count == 0 ? "" : ", ",
                               pin, level ? "HIGH" : "LOW");
            if (written < 0 || (size_t)written >= remaining) break;
            cursor += (size_t)written;
            remaining -= (size_t)written;
            count++;
        }
    }

    if (count == 0) {
        snprintf(output, output_size, "Error: no allowed GPIO pins configured");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "gpio_read_all: %d pins read", count);
    return ESP_OK;
>>>>>>> bb10ea0149080d506d920c09054f4c5b20409de2
}
