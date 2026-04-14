#include "tools/tool_sgp30.h"
#include "mimi_config.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "tool_sgp30";

#define SGP30_I2C_ADDRESS          0x58
#define SGP30_WORD_CRC_POLY        0x31
#define SGP30_WORD_CRC_INIT        0xFF
#define SGP30_WARMUP_US            (15LL * 1000 * 1000)
#define SGP30_MEASURE_INTERVAL_US  (1LL * 1000 * 1000)
#define SGP30_XFER_TIMEOUT_MS      100

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    int sda_gpio;
    int scl_gpio;
    int i2c_port;
    int scl_hz;
    int64_t init_us;
    int64_t last_measure_us;
    uint16_t last_co2eq_ppm;
    uint16_t last_tvoc_ppb;
    bool has_sample;
} sgp30_state_t;

static sgp30_state_t s_sgp30 = {
    .bus = NULL,
    .dev = NULL,
    .sda_gpio = -1,
    .scl_gpio = -1,
    .i2c_port = -1,
    .scl_hz = 0,
    .init_us = 0,
    .last_measure_us = 0,
    .last_co2eq_ppm = 0,
    .last_tvoc_ppb = 0,
    .has_sample = false,
};

/**
 * @brief 从工具输入 JSON 中读取一个可选的整数参数。
 *
 * @param root 已解析完成的 cJSON 根对象。
 * @param key 要读取的字段名。
 * @param value 用于输出解析结果的整数指针。
 *
 * @return 若字段存在且为数值类型则返回 true，否则返回 false。
 */
static bool get_optional_int(cJSON *root, const char *key, int *value)
{
    cJSON *item = cJSON_GetObjectItem(root, key);
    if (!item || !cJSON_IsNumber(item)) {
        return false;
    }

    *value = item->valueint;
    return true;
}

/**
 * @brief 计算 SGP30 单个 2 字节数据字对应的 CRC 校验值。
 *
 * SGP30 使用 CRC-8，生成多项式为 `0x31`，初始值为 `0xFF`。
 *
 * @param data 指向恰好 2 个数据字节的指针。
 *
 * @return 对应这 2 个字节计算得到的 CRC 值。
 */
static uint8_t sgp30_crc_word(const uint8_t *data)
{
    uint8_t crc = SGP30_WORD_CRC_INIT;
    for (int i = 0; i < 2; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x80) {
                crc = (uint8_t)((crc << 1) ^ SGP30_WORD_CRC_POLY);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/**
 * @brief 检查给定 GPIO 组合是否可作为 SDA 和 SCL 使用。
 *
 * @param sda_gpio 候选 SDA GPIO 编号。
 * @param scl_gpio 候选 SCL GPIO 编号。
 *
 * @return 如果两个 GPIO 在当前芯片上都有效则返回 true。
 */
static bool valid_gpio_pair(int sda_gpio, int scl_gpio)
{
    return GPIO_IS_VALID_GPIO(sda_gpio) && GPIO_IS_VALID_GPIO(scl_gpio);
}

/**
 * @brief 释放缓存的 SGP30 I2C 总线和设备句柄，并重置缓存状态。
 *
 * 当引脚或总线配置发生变化，或者初始化过程中失败需要清理半成品状态时，
 * 会调用该函数完成资源释放和状态复位。
 */
static void sgp30_release(void)
{
    if (s_sgp30.dev) {
        i2c_master_bus_rm_device(s_sgp30.dev);
        s_sgp30.dev = NULL;
    }
    if (s_sgp30.bus) {
        i2c_del_master_bus(s_sgp30.bus);
        s_sgp30.bus = NULL;
    }
    s_sgp30.sda_gpio = -1;
    s_sgp30.scl_gpio = -1;
    s_sgp30.i2c_port = -1;
    s_sgp30.scl_hz = 0;
    s_sgp30.init_us = 0;
    s_sgp30.last_measure_us = 0;
    s_sgp30.last_co2eq_ppm = 0;
    s_sgp30.last_tvoc_ppb = 0;
    s_sgp30.has_sample = false;
}

/**
 * @brief 向当前已接入的 SGP30 设备发送一个 16 位命令字。
 *
 * @param command SGP30 命令集中的 16 位大端命令字。
 *
 * @return `i2c_master_transmit()` 返回的 ESP-IDF 传输状态码。
 */
static esp_err_t sgp30_send_command(uint16_t command)
{
    uint8_t cmd[2] = {
        (uint8_t)(command >> 8),
        (uint8_t)(command & 0xFF),
    };
    return i2c_master_transmit(s_sgp30.dev, cmd, sizeof(cmd), SGP30_XFER_TIMEOUT_MS);
}

/**
 * @brief 从 SGP30 读取一次 IAQ 采样结果，并校验两个数据字的 CRC。
 *
 * 命令流程为发送 `measure_iaq (0x2008)`，随后读取 6 字节响应：
 * `CO2eq_MSB`、`CO2eq_LSB`、`CO2eq_CRC`、`TVOC_MSB`、`TVOC_LSB`、`TVOC_CRC`。
 *
 * @param co2eq_ppm 输出 eCO2 估算值，单位 ppm。
 * @param tvoc_ppb 输出 TVOC 估算值，单位 ppb。
 *
 * @retval ESP_OK 采样读取成功且 CRC 校验通过。
 * @retval ESP_ERR_INVALID_CRC 至少一个返回数据字的 CRC 校验失败。
 * @retval 其他 ESP-IDF 错误码 I2C 事务执行失败。
 */
static esp_err_t sgp30_measure(uint16_t *co2eq_ppm, uint16_t *tvoc_ppb)
{
    uint8_t cmd[2] = { 0x20, 0x08 };
    uint8_t resp[6] = {0};

    esp_err_t err = i2c_master_transmit_receive(s_sgp30.dev, cmd, sizeof(cmd), resp, sizeof(resp), SGP30_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    if (sgp30_crc_word(&resp[0]) != resp[2] || sgp30_crc_word(&resp[3]) != resp[5]) {
        return ESP_ERR_INVALID_CRC;
    }

    *co2eq_ppm = (uint16_t)((resp[0] << 8) | resp[1]);
    *tvoc_ppb = (uint16_t)((resp[3] << 8) | resp[4]);
    return ESP_OK;
}

/**
 * @brief 确保缓存中的 SGP30 总线和设备句柄与当前请求的总线参数一致。
 *
 * 如果当前缓存的句柄已经匹配所请求的 GPIO、I2C 端口和时钟频率，
 * 则直接返回。否则会先释放旧状态，再重新创建 I2C 主机总线，
 * 探测地址 `0x58`，添加设备句柄，并发送 `iaq_init` 初始化命令。
 *
 * @param sda_gpio SGP30 所使用的 SDA GPIO 编号。
 * @param scl_gpio SGP30 所使用的 SCL GPIO 编号。
 * @param i2c_port I2C 控制器编号，或 `-1` 表示使用默认配置。
 * @param scl_hz I2C 时钟频率，单位 Hz。
 *
 * @retval ESP_OK 设备已完成初始化，可以开始测量。
 * @retval ESP_ERR_INVALID_ARG GPIO 或时钟参数非法。
 * @retval ESP_ERR_NOT_FOUND 地址 `0x58` 上没有设备应答。
 * @retval 其他 ESP-IDF 错误码 总线创建、设备注册或初始化失败。
 */
static esp_err_t sgp30_ensure_ready(int sda_gpio, int scl_gpio, int i2c_port, int scl_hz)
{
    if (!valid_gpio_pair(sda_gpio, scl_gpio)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (scl_hz <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_sgp30.bus && s_sgp30.dev &&
        s_sgp30.sda_gpio == sda_gpio &&
        s_sgp30.scl_gpio == scl_gpio &&
        s_sgp30.i2c_port == i2c_port &&
        s_sgp30.scl_hz == scl_hz) {
        return ESP_OK;
    }

    if (s_sgp30.bus || s_sgp30.dev) {
        sgp30_release();
    }

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = i2c_port,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = 1,
    };

    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_sgp30.bus);
    if (err != ESP_OK) {
        sgp30_release();
        return err;
    }

    err = i2c_master_probe(s_sgp30.bus, SGP30_I2C_ADDRESS, SGP30_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        sgp30_release();
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SGP30_I2C_ADDRESS,
        .scl_speed_hz = (uint32_t)scl_hz,
        .scl_wait_us = 0,
    };

    err = i2c_master_bus_add_device(s_sgp30.bus, &dev_cfg, &s_sgp30.dev);
    if (err != ESP_OK) {
        sgp30_release();
        return err;
    }

    err = sgp30_send_command(0x2003);
    if (err != ESP_OK) {
        sgp30_release();
        return err;
    }

    s_sgp30.sda_gpio = sda_gpio;
    s_sgp30.scl_gpio = scl_gpio;
    s_sgp30.i2c_port = i2c_port;
    s_sgp30.scl_hz = scl_hz;
    s_sgp30.init_us = esp_timer_get_time();
    return ESP_OK;
}

/**
 * @brief 执行 MimiClaw 的 SGP30 空气质量读取工具。
 *
 * 该函数会从 JSON 输入中解析可选的总线覆盖参数，校验 GPIO 配置，
 * 在首次使用时初始化 I2C 总线，缓存总线/设备句柄以及最近一次采样结果，
 * 最终返回预热状态文本或正常空气质量读数文本。
 *
 * @param input_json 包含可选 `sda_gpio`、`scl_gpio`、`i2c_port` 和 `scl_hz`
 * 覆盖参数的 JSON 字符串。
 * @param output 调用方提供的字符串缓冲区，用于接收最终工具返回文本。
 * @param output_size @p output 缓冲区大小，单位为字节。
 *
 * @retval ESP_OK 工具执行成功，@p output 中包含状态文本。
 * @retval ESP_ERR_INVALID_ARG 输入 JSON 或总线参数非法。
 * @retval ESP_ERR_INVALID_STATE 没有可用的 SDA/SCL 配置。
 * @retval ESP_ERR_NOT_FOUND 请求的总线上未发现 SGP30。
 * @retval ESP_ERR_INVALID_CRC 传感器返回数据的 CRC 校验失败。
 * @retval 其他 ESP-IDF 错误码 初始化或传输过程失败。
 */
esp_err_t tool_sgp30_read_air_quality_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    int sda_gpio = MIMI_SGP30_DEFAULT_SDA_GPIO;
    int scl_gpio = MIMI_SGP30_DEFAULT_SCL_GPIO;
    int i2c_port = MIMI_SGP30_DEFAULT_I2C_PORT;
    int scl_hz = MIMI_SGP30_DEFAULT_SCL_HZ;

    (void)get_optional_int(root, "sda_gpio", &sda_gpio);
    (void)get_optional_int(root, "scl_gpio", &scl_gpio);
    (void)get_optional_int(root, "i2c_port", &i2c_port);
    (void)get_optional_int(root, "scl_hz", &scl_hz);

    if (!valid_gpio_pair(sda_gpio, scl_gpio)) {
        snprintf(output, output_size,
                 "Error: SGP30 SDA/SCL GPIOs are not configured. Set MIMI_SECRET_SGP30_SDA_GPIO and MIMI_SECRET_SGP30_SCL_GPIO, or call with {\"sda_gpio\":x,\"scl_gpio\":y}.");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = sgp30_ensure_ready(sda_gpio, scl_gpio, i2c_port, scl_hz);
    if (err != ESP_OK) {
        if (err == ESP_ERR_NOT_FOUND) {
            snprintf(output, output_size,
                     "Error: SGP30 not found at I2C address 0x58 on SDA=%d SCL=%d", sda_gpio, scl_gpio);
        } else {
            snprintf(output, output_size,
                     "Error: failed to initialize SGP30 on SDA=%d SCL=%d (%s)",
                     sda_gpio, scl_gpio, esp_err_to_name(err));
        }
        cJSON_Delete(root);
        return err;
    }

    int64_t now_us = esp_timer_get_time();
    bool used_cache = false;
    if (s_sgp30.has_sample && (now_us - s_sgp30.last_measure_us) < SGP30_MEASURE_INTERVAL_US) {
        used_cache = true;
    } else {
        uint16_t co2eq_ppm = 0;
        uint16_t tvoc_ppb = 0;
        err = sgp30_measure(&co2eq_ppm, &tvoc_ppb);
        if (err != ESP_OK) {
            snprintf(output, output_size, "Error: failed to read SGP30 sample (%s)", esp_err_to_name(err));
            cJSON_Delete(root);
            return err;
        }

        s_sgp30.last_co2eq_ppm = co2eq_ppm;
        s_sgp30.last_tvoc_ppb = tvoc_ppb;
        s_sgp30.last_measure_us = now_us;
        s_sgp30.has_sample = true;
    }

    int64_t warmup_remaining_us = SGP30_WARMUP_US - (now_us - s_sgp30.init_us);
    if (warmup_remaining_us > 0) {
        snprintf(output, output_size,
                 "OK: SGP30 warming up on SDA=%d SCL=%d. CO2eq=%u ppm, TVOC=%u ppb, %.1f s remaining%s",
                 sda_gpio, scl_gpio,
                 (unsigned)s_sgp30.last_co2eq_ppm,
                 (unsigned)s_sgp30.last_tvoc_ppb,
                 (double)warmup_remaining_us / 1000000.0,
                 used_cache ? " (cached)" : "");
    } else {
        snprintf(output, output_size,
                 "OK: SGP30 air quality on SDA=%d SCL=%d -> CO2eq=%u ppm, TVOC=%u ppb%s",
                 sda_gpio, scl_gpio,
                 (unsigned)s_sgp30.last_co2eq_ppm,
                 (unsigned)s_sgp30.last_tvoc_ppb,
                 used_cache ? " (cached)" : "");
    }

    ESP_LOGI(TAG, "sgp30_read_air_quality sda=%d scl=%d port=%d freq=%d -> %s",
             sda_gpio, scl_gpio, i2c_port, scl_hz, output);
    cJSON_Delete(root);
    return ESP_OK;
}
