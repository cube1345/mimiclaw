#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
 * Set a GPIO pin to digital high or low output.
 * Input JSON: { pin, level }
 */
esp_err_t tool_gpio_write_execute(const char *input_json, char *output, size_t output_size);

/**
 * Set a single WS2812/NeoPixel LED color.
 * Input JSON: { r, g, b, pin?, brightness? }
 */
esp_err_t tool_ws2812_set_execute(const char *input_json, char *output, size_t output_size);
