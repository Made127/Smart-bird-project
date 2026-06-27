#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "SoilProbe.h"
#include "bird_test_config.h"

// 土壤探头模块通过 UART/Modbus RTU 读取温湿度、电导率、盐分、NPK 和 pH。
static const char *TAG = "SoilProbe";
static soil_probe_data_t latest_data;
static SemaphoreHandle_t probe_lock;
static bool probe_ready;
static int soil_probe_de_pin = -1;

// 计算 Modbus RTU CRC16，低字节先发送。
static uint16_t modbus_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if ((crc & 0x0001) != 0) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

// 从大端字节序缓冲区读取 16 位寄存器值。
static uint16_t read_u16_be(const uint8_t *buf)
{
    return ((uint16_t)buf[0] << 8) | (uint16_t)buf[1];
}

// 向指定地址发送 0x03 读保持寄存器命令，并校验返回帧。
static esp_err_t soil_probe_query(uint8_t addr, uint8_t *response, size_t response_len)
{
    uint8_t request[8] = {
        addr,
        0x03,
        0x00,
        0x00,
        0x00,
        0x08,
        0x00,
        0x00,
    };
    // 这行很重要：Modbus RTU 帧必须带 CRC，且只计算前 6 个请求字节。
    uint16_t request_crc = modbus_crc16(request, 6);
    request[6] = request_crc & 0xFF;
    request[7] = request_crc >> 8;

    uart_flush_input(BIRD_SOIL_PROBE_UART);

    if (soil_probe_de_pin >= 0) {
        gpio_set_level(soil_probe_de_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    ESP_LOGI(TAG, "query addr=0x%02x frame=%02x %02x %02x %02x %02x %02x %02x %02x",
             addr,
             request[0], request[1], request[2], request[3],
             request[4], request[5], request[6], request[7]);
    int written = uart_write_bytes(BIRD_SOIL_PROBE_UART, request, sizeof(request));
    uart_wait_tx_done(BIRD_SOIL_PROBE_UART, pdMS_TO_TICKS(100));

    if (soil_probe_de_pin >= 0) {
        vTaskDelay(pdMS_TO_TICKS(2));
        gpio_set_level(soil_probe_de_pin, 0);
    }

    if (written != sizeof(request)) {
        ESP_LOGW(TAG, "query write failed, written=%d", written);
        return ESP_FAIL;
    }

    int total = 0;
    int empty_reads = 0;
    while (total < (int)response_len && empty_reads < 20) {
        int read_len = uart_read_bytes(
            BIRD_SOIL_PROBE_UART,
            response + total,
            response_len - total,
            pdMS_TO_TICKS(50));
        if (read_len > 0) {
            total += read_len;
            empty_reads = 0;
        } else {
            empty_reads++;
        }
    }

    if (total < (int)response_len) {
        ESP_LOGW(TAG, "response timeout addr=0x%02x, bytes=%d", addr, total);
        return ESP_ERR_TIMEOUT;
    }

    uint16_t expected_crc = modbus_crc16(response, response_len - 2);
    uint16_t actual_crc = (uint16_t)response[response_len - 2] |
                          ((uint16_t)response[response_len - 1] << 8);
    // 这行很重要：地址、功能码、字节数和 CRC 任一不匹配，都不能解析为有效土壤数据。
    if (response[0] != addr ||
        response[1] != 0x03 ||
        response[2] != 16 ||
        expected_crc != actual_crc) {
        ESP_LOGW(TAG,
                 "bad response addr=0x%02x got=0x%02x func=0x%02x bytes=%u crc=%04x/%04x",
                 addr,
                 response[0],
                 response[1],
                 response[2],
                 actual_crc,
                 expected_crc);
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

// 初始化土壤探头 UART；如配置了 DE 脚，则同时支持 RS485 方向控制。
esp_err_t SoilProbe_init(void)
{
#if BIRD_SOIL_PROBE_ENABLED
    probe_lock = xSemaphoreCreateMutex();
    if (!probe_lock) {
        return ESP_ERR_NO_MEM;
    }

    uart_config_t config = {
        .baud_rate = BIRD_SOIL_PROBE_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(BIRD_SOIL_PROBE_UART, 256, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(uart_param_config(BIRD_SOIL_PROBE_UART, &config));
    ESP_ERROR_CHECK(uart_set_pin(
        BIRD_SOIL_PROBE_UART,
        BIRD_SOIL_PROBE_TX_PIN,
        BIRD_SOIL_PROBE_RX_PIN,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE));

    soil_probe_de_pin = BIRD_SOIL_PROBE_DE_PIN;
    if (soil_probe_de_pin >= 0) {
        gpio_config_t de_config = {
            .pin_bit_mask = 1ULL << soil_probe_de_pin,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&de_config));
        gpio_set_level(soil_probe_de_pin, 0);
    }

    memset(&latest_data, 0, sizeof(latest_data));
    probe_ready = true;
    ESP_LOGI(TAG, "ready addr=0x%02x baud=%d tx=%d rx=%d de=%d",
             BIRD_SOIL_PROBE_ADDR,
             BIRD_SOIL_PROBE_BAUD,
             BIRD_SOIL_PROBE_TX_PIN,
             BIRD_SOIL_PROBE_RX_PIN,
             BIRD_SOIL_PROBE_DE_PIN);
    return ESP_OK;
#else
    ESP_LOGI(TAG, "disabled");
    return ESP_OK;
#endif
}

// 主动读取一次探头数据，失败时会尝试常见默认地址 0x01。
esp_err_t SoilProbe_read_once(void)
{
#if BIRD_SOIL_PROBE_ENABLED
    if (!probe_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(probe_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t response[21] = {0};
    esp_err_t err = soil_probe_query(BIRD_SOIL_PROBE_ADDR, response, sizeof(response));
    // 这行很重要：很多探头出厂地址是 0x01，配置地址失败时自动试一次常见地址。
    if (err != ESP_OK && BIRD_SOIL_PROBE_ADDR != 0x01) {
        memset(response, 0, sizeof(response));
        ESP_LOGW(TAG, "addr=0x%02x failed, try common addr=0x01",
                 BIRD_SOIL_PROBE_ADDR);
        err = soil_probe_query(0x01, response, sizeof(response));
    }
    if (err != ESP_OK) {
        xSemaphoreGive(probe_lock);
        return err;
    }

    // 这行很重要：响应寄存器顺序必须和探头协议一致，改探头型号时要先核对协议表。
    soil_probe_data_t data = {
        .valid = true,
        .temperature_x10 = (int16_t)read_u16_be(&response[3]),
        .humidity_x10 = read_u16_be(&response[5]),
        .ec_us_cm = read_u16_be(&response[7]),
        .salt_ppm = read_u16_be(&response[9]),
        .nitrogen_mg_kg = read_u16_be(&response[11]),
        .phosphorus_mg_kg = read_u16_be(&response[13]),
        .potassium_mg_kg = read_u16_be(&response[15]),
        .ph_x10 = read_u16_be(&response[17]),
        .updated_ms = esp_timer_get_time() / 1000,
    };

    latest_data = data;
    xSemaphoreGive(probe_lock);

    ESP_LOGI(TAG,
             "temp=%.1f hum=%.1f ec=%u salt=%u n=%u p=%u k=%u ph=%.1f",
             data.temperature_x10 / 10.0f,
             data.humidity_x10 / 10.0f,
             data.ec_us_cm,
             data.salt_ppm,
             data.nitrogen_mg_kg,
             data.phosphorus_mg_kg,
             data.potassium_mg_kg,
             data.ph_x10 / 10.0f);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

// 复制最近一次有效土壤数据，供 AI 上报和服务器查询使用。
bool SoilProbe_get_latest(soil_probe_data_t *out)
{
    if (!out || !probe_lock) {
        return false;
    }
    bool valid = false;
    if (xSemaphoreTake(probe_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        *out = latest_data;
        valid = latest_data.valid;
        xSemaphoreGive(probe_lock);
    }
    return valid;
}
