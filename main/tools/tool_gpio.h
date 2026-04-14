#pragma once

#include "esp_err.h"
#include <stddef.h>

/**
<<<<<<< HEAD
 * Set a GPIO pin to digital high or low output.
 * Input JSON: { pin, level }
=======
 * Initialize GPIO tool — configure allowed pins and directions.
 */
esp_err_t tool_gpio_init(void);

/**
 * Write a GPIO pin HIGH or LOW.
 * Input JSON: {"pin": <int>, "state": <0|1>}
>>>>>>> bb10ea0149080d506d920c09054f4c5b20409de2
 */
esp_err_t tool_gpio_write_execute(const char *input_json, char *output, size_t output_size);

/**
<<<<<<< HEAD
 * Set a single WS2812/NeoPixel LED color.
 * Input JSON: { r, g, b, pin?, brightness? }
 */
esp_err_t tool_ws2812_set_execute(const char *input_json, char *output, size_t output_size);
=======
 * Read a single GPIO pin state.
 * Input JSON: {"pin": <int>}
 */
esp_err_t tool_gpio_read_execute(const char *input_json, char *output, size_t output_size);

/**
 * Read all allowed GPIO pin states at once.
 * Input JSON: {} (no parameters)
 */
esp_err_t tool_gpio_read_all_execute(const char *input_json, char *output, size_t output_size);
>>>>>>> bb10ea0149080d506d920c09054f4c5b20409de2
