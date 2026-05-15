#include "sensors/sensor_mqtt.h"

#include "mimi_config.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *TAG = "sensor_mqtt";

#define DHT22_MAX_RETRIES      3
#define MQTT_BUF_SIZE          512
#define MQTT_CLIENT_ID         "mimiclaw-esp32"
#define MQTT_KEEPALIVE_S       30
#define MHZ19_CMD_LEN          9
#define MHZ19_UART_BUF_SIZE    128

static portMUX_TYPE s_dht22_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_mhz19_uart_ready = false;

static int wait_level_timeout(int pin, int level, int timeout_us)
{
    int64_t start = esp_timer_get_time();
    while (gpio_get_level((gpio_num_t)pin) != level) {
        if ((esp_timer_get_time() - start) >= timeout_us) {
            return -1;
        }
    }
    return 0;
}

static esp_err_t dht22_read(float *temperature, float *humidity)
{
    const int pin = MIMI_SENSOR_DHT22_GPIO;
    uint8_t data[5] = {0};

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "configure DHT22 output failed");

    gpio_set_level((gpio_num_t)pin, 0);
    esp_rom_delay_us(2000);
    gpio_set_level((gpio_num_t)pin, 1);
    esp_rom_delay_us(40);

    cfg.mode = GPIO_MODE_INPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "configure DHT22 input failed");

    portENTER_CRITICAL(&s_dht22_mux);

    if (wait_level_timeout(pin, 0, 1000) != 0 ||
        wait_level_timeout(pin, 1, 1000) != 0 ||
        wait_level_timeout(pin, 0, 1000) != 0) {
        portEXIT_CRITICAL(&s_dht22_mux);
        return ESP_ERR_TIMEOUT;
    }

    for (int byte = 0; byte < 5; byte++) {
        uint8_t value = 0;
        for (int bit = 0; bit < 8; bit++) {
            if (wait_level_timeout(pin, 1, 1000) != 0) {
                portEXIT_CRITICAL(&s_dht22_mux);
                return ESP_ERR_TIMEOUT;
            }
            int64_t t0 = esp_timer_get_time();
            if (wait_level_timeout(pin, 0, 1000) != 0) {
                portEXIT_CRITICAL(&s_dht22_mux);
                return ESP_ERR_TIMEOUT;
            }
            value <<= 1;
            if ((esp_timer_get_time() - t0) > 50) {
                value |= 1;
            }
        }
        data[byte] = value;
    }

    portEXIT_CRITICAL(&s_dht22_mux);

    uint8_t checksum = (uint8_t)(data[0] + data[1] + data[2] + data[3]);
    if (checksum != data[4]) {
        return ESP_ERR_INVALID_CRC;
    }

    uint16_t raw_humidity = ((uint16_t)data[0] << 8) | data[1];
    uint16_t raw_temp = ((uint16_t)data[2] << 8) | data[3];
    float temp = (float)(raw_temp & 0x7FFF) / 10.0f;
    if (raw_temp & 0x8000) {
        temp = -temp;
    }

    *humidity = (float)raw_humidity / 10.0f;
    *temperature = temp;
    return ESP_OK;
}

static esp_err_t read_dht22_with_retry(float *temperature, float *humidity)
{
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < DHT22_MAX_RETRIES; i++) {
        err = dht22_read(temperature, humidity);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return err;
}

static uint8_t mhz19_checksum(const uint8_t *packet)
{
    uint8_t sum = 0;
    for (int i = 1; i < 8; i++) {
        sum = (uint8_t)(sum + packet[i]);
    }
    return (uint8_t)(0xFF - sum + 1);
}

static esp_err_t mhz19_uart_init(void)
{
    if (s_mhz19_uart_ready) {
        return ESP_OK;
    }

    if (MIMI_SENSOR_MHZ19_RX_GPIO < 0 || MIMI_SENSOR_MHZ19_TX_GPIO < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uart_config_t cfg = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_port_t uart_num = (uart_port_t)MIMI_SENSOR_MHZ19_UART_NUM;
    ESP_RETURN_ON_ERROR(uart_driver_install(uart_num, MHZ19_UART_BUF_SIZE, 0, 0, NULL, 0),
                        TAG, "install MH-Z19 UART failed");
    ESP_RETURN_ON_ERROR(uart_param_config(uart_num, &cfg), TAG, "configure MH-Z19 UART failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(uart_num,
                                     MIMI_SENSOR_MHZ19_TX_GPIO,
                                     MIMI_SENSOR_MHZ19_RX_GPIO,
                                     UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG, "set MH-Z19 UART pins failed");

    s_mhz19_uart_ready = true;
    return ESP_OK;
}

static esp_err_t mhz19_read_co2(int *co2_ppm)
{
    ESP_RETURN_ON_ERROR(mhz19_uart_init(), TAG, "MH-Z19 UART unavailable");

    static const uint8_t cmd[MHZ19_CMD_LEN] = {0xFF, 0x01, 0x86, 0, 0, 0, 0, 0, 0x79};
    uint8_t resp[MHZ19_CMD_LEN] = {0};
    uart_port_t uart_num = (uart_port_t)MIMI_SENSOR_MHZ19_UART_NUM;

    uart_flush_input(uart_num);
    int written = uart_write_bytes(uart_num, (const char *)cmd, sizeof(cmd));
    if (written != sizeof(cmd)) {
        return ESP_FAIL;
    }

    int n = uart_read_bytes(uart_num, resp, sizeof(resp), pdMS_TO_TICKS(1000));
    if (n != sizeof(resp)) {
        return ESP_ERR_TIMEOUT;
    }
    if (resp[0] != 0xFF || resp[1] != 0x86 || mhz19_checksum(resp) != resp[8]) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *co2_ppm = ((int)resp[2] << 8) | resp[3];
    return ESP_OK;
}

static int write_all(int fd, const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int n = send(fd, data + off, len - off, 0);
        if (n <= 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static size_t mqtt_encode_remaining_len(uint8_t *out, size_t value)
{
    size_t count = 0;
    do {
        uint8_t byte = (uint8_t)(value % 128);
        value /= 128;
        if (value > 0) {
            byte |= 0x80;
        }
        out[count++] = byte;
    } while (value > 0 && count < 4);
    return count;
}

static uint8_t *mqtt_write_string(uint8_t *p, const char *s)
{
    size_t len = strlen(s);
    *p++ = (uint8_t)(len >> 8);
    *p++ = (uint8_t)(len & 0xFF);
    memcpy(p, s, len);
    return p + len;
}

static int mqtt_send_connect(int fd)
{
    uint8_t payload[128];
    uint8_t *p = payload;
    p = mqtt_write_string(p, "MQTT");
    *p++ = 4;
    *p++ = 0x02;
    *p++ = 0;
    *p++ = MQTT_KEEPALIVE_S;
    p = mqtt_write_string(p, MQTT_CLIENT_ID);

    uint8_t packet[160];
    packet[0] = 0x10;
    size_t rem_len_len = mqtt_encode_remaining_len(&packet[1], (size_t)(p - payload));
    memcpy(packet + 1 + rem_len_len, payload, (size_t)(p - payload));
    return write_all(fd, packet, 1 + rem_len_len + (size_t)(p - payload));
}

static int mqtt_read_connack(int fd)
{
    uint8_t resp[4] = {0};
    int n = recv(fd, resp, sizeof(resp), MSG_WAITALL);
    if (n != sizeof(resp)) {
        return -1;
    }
    return (resp[0] == 0x20 && resp[1] == 0x02 && resp[2] == 0x00 && resp[3] == 0x00) ? 0 : -1;
}

static int mqtt_subscribe(int fd, const char *topic, uint16_t packet_id)
{
    uint8_t payload[160];
    uint8_t *p = payload;
    *p++ = (uint8_t)(packet_id >> 8);
    *p++ = (uint8_t)(packet_id & 0xFF);
    p = mqtt_write_string(p, topic);
    *p++ = 0;

    uint8_t packet[192];
    packet[0] = 0x82;
    size_t rem_len_len = mqtt_encode_remaining_len(&packet[1], (size_t)(p - payload));
    memcpy(packet + 1 + rem_len_len, payload, (size_t)(p - payload));
    return write_all(fd, packet, 1 + rem_len_len + (size_t)(p - payload));
}

static int mqtt_publish(int fd, const char *topic, const char *payload)
{
    size_t topic_len = strlen(topic);
    size_t payload_len = strlen(payload);
    size_t rem_len = 2 + topic_len + payload_len;
    if (rem_len + 5 > MQTT_BUF_SIZE) {
        return -1;
    }

    uint8_t packet[MQTT_BUF_SIZE];
    packet[0] = 0x30;
    size_t rem_len_len = mqtt_encode_remaining_len(&packet[1], rem_len);
    uint8_t *p = packet + 1 + rem_len_len;
    p = mqtt_write_string(p, topic);
    memcpy(p, payload, payload_len);
    return write_all(fd, packet, 1 + rem_len_len + 2 + topic_len + payload_len);
}

static int mqtt_connect_tcp(void)
{
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", MIMI_SENSOR_MQTT_PORT);

    int err = getaddrinfo(MIMI_SENSOR_MQTT_BROKER, port_str, &hints, &res);
    if (err != 0 || !res) {
        ESP_LOGW(TAG, "MQTT broker resolve failed: %s", MIMI_SENSOR_MQTT_BROKER);
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    struct timeval tv = {
        .tv_sec = 3,
        .tv_usec = 0,
    };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGW(TAG, "MQTT TCP connect failed: errno=%d", errno);
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static void mqtt_poll_inbound(int fd)
{
    uint8_t header[2] = {0};
    int n = recv(fd, header, sizeof(header), MSG_DONTWAIT);
    if (n <= 0) {
        return;
    }

    uint8_t packet_type = header[0] & 0xF0;
    size_t remaining = header[1];
    uint8_t payload[MQTT_BUF_SIZE];
    if (remaining >= sizeof(payload)) {
        return;
    }
    n = recv(fd, payload, remaining, MSG_WAITALL);
    if (n != (int)remaining) {
        return;
    }

    if (packet_type == 0x30 && remaining >= 2) {
        size_t topic_len = ((size_t)payload[0] << 8) | payload[1];
        if (2 + topic_len < remaining) {
            const char *msg = (const char *)&payload[2 + topic_len];
            size_t msg_len = remaining - 2 - topic_len;
            ESP_LOGI(TAG, "MQTT inbound %.*s: %.*s",
                     (int)topic_len, (const char *)&payload[2],
                     (int)msg_len, msg);
        }
    }
}

static esp_err_t publish_sensor_data(int fd)
{
    float temp = NAN;
    float humidity = NAN;
    int co2 = 0;

    esp_err_t dht_err = read_dht22_with_retry(&temp, &humidity);
    esp_err_t co2_err = mhz19_read_co2(&co2);
    if (dht_err != ESP_OK || co2_err != ESP_OK) {
        ESP_LOGW(TAG, "skip MQTT publish: DHT22=%s MH-Z19=%s",
                 esp_err_to_name(dht_err), esp_err_to_name(co2_err));
        return ESP_FAIL;
    }

    char json[128];
    snprintf(json, sizeof(json),
             "{\"temp\":%.1f,\"humidity\":%.1f,\"co2\":%d}",
             (double)temp, (double)humidity, co2);

    if (mqtt_publish(fd, MIMI_SENSOR_MQTT_TOPIC_DATA, json) != 0) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "MQTT publish %s: %s", MIMI_SENSOR_MQTT_TOPIC_DATA, json);
    return ESP_OK;
}

static void sensor_mqtt_task(void *arg)
{
    (void)arg;

    while (1) {
        int fd = mqtt_connect_tcp();
        if (fd < 0 || mqtt_send_connect(fd) != 0 || mqtt_read_connack(fd) != 0) {
            ESP_LOGW(TAG, "MQTT connect failed");
            if (fd >= 0) {
                close(fd);
            }
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        ESP_LOGI(TAG, "MQTT connected to %s:%d", MIMI_SENSOR_MQTT_BROKER, MIMI_SENSOR_MQTT_PORT);
        mqtt_subscribe(fd, MIMI_SENSOR_MQTT_TOPIC_ANALYSIS, 1);
        mqtt_subscribe(fd, MIMI_SENSOR_MQTT_TOPIC_ALERT, 2);

        while (1) {
            mqtt_poll_inbound(fd);
            if (publish_sensor_data(fd) != ESP_OK) {
                ESP_LOGW(TAG, "MQTT publish failed, reconnecting");
                close(fd);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(MIMI_SENSOR_MQTT_PUBLISH_INTERVAL_MS));
        }
    }
}

esp_err_t sensor_mqtt_start(void)
{
    if (MIMI_SENSOR_MQTT_BROKER[0] == '\0') {
        ESP_LOGI(TAG, "Sensor MQTT disabled: MIMI_SECRET_SENSOR_MQTT_BROKER is empty");
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(sensor_mqtt_task, "sensor_mqtt",
                                           MIMI_SENSOR_MQTT_STACK, NULL,
                                           MIMI_SENSOR_MQTT_PRIO, NULL,
                                           MIMI_SENSOR_MQTT_CORE);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
