#include "LS.h"

#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// BH1750 光照传感器命令字。
#define BH1750_POWER_ON 0x01
#define BH1750_CONTINUOUS_HIGH_RES_MODE 0x10

// 光照模块通过 I2C 读取 BH1750，并换算为 lux。
static const char *TAG = "LS";
static int light_sensor_ready;

// 初始化 I2C 总线并把 BH1750 切到连续高分辨率模式。
esp_err_t LS_init(void)
{
    i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BH1750_SDA,
        .scl_io_num = BH1750_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
        .clk_flags = 0,
    };

    esp_err_t err = i2c_param_config(I2C_Port, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_driver_install(I2C_Port, config.mode, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "I2C install failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t command = BH1750_POWER_ON;
    err = i2c_master_write_to_device(
        I2C_Port, BH1750_ADDR, &command, 1, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BH1750 power-on failed: %s", esp_err_to_name(err));
        return err;
    }

    command = BH1750_CONTINUOUS_HIGH_RES_MODE;
    err = i2c_master_write_to_device(
        I2C_Port, BH1750_ADDR, &command, 1, pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BH1750 mode setup failed: %s", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(180));
    light_sensor_ready = 1;
    ESP_LOGI(TAG, "ready, SDA=%d, SCL=%d, address=0x%02x",
             BH1750_SDA, BH1750_SCL, BH1750_ADDR);
    return ESP_OK;
}

// 读取当前光照强度；传感器未就绪或读取失败时返回 -1。
float LS_Get_Lux(void)
{
    if (!light_sensor_ready) {
        return -1.0f;
    }

    uint8_t data[2] = {0};
    esp_err_t err = i2c_master_read_from_device(
        I2C_Port, BH1750_ADDR, data, sizeof(data), pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(err));
        return -1.0f;
    }

    uint16_t raw = ((uint16_t)data[0] << 8) | data[1];
    return (float)raw / 1.2f;
}
