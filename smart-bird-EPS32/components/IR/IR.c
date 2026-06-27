#include "IR.h"

#include "esp_log.h"

// 红外人体感应模块：读取单个数字输入脚的原始电平。
static const char *TAG = "IR";

// 初始化红外传感器 GPIO 输入。
esp_err_t IR_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << IR_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ready, pin=%d, raw=%d", IR_PIN, IR_read());
    } else {
        ESP_LOGE(TAG, "init failed: %s", esp_err_to_name(err));
    }
    return err;
}

// 返回红外传感器原始电平，具体高/低含义取决于模块输出方式。
int IR_read(void)
{
    return gpio_get_level(IR_PIN);
}

// 当前配置按高电平表示检测到人体。
int IR_has_person(void)
{
    return IR_read() == 1;
}
