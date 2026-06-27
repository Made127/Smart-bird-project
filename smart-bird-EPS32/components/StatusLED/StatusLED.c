#include "StatusLED.h"

#include <stdint.h>

#include "bird_test_config.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// RGB 状态灯模块用 PWM 显示网络、电池和强制熄灭状态。
#define LED_PWM_MAX 255

static const char *TAG = "StatusLED";
static status_led_network_t network_status = STATUS_LED_INITIALIZING;
static bool charging;
static bool low_battery;
static bool battery_full;
static bool force_off;

// 根据 RGB 模块是高电平有效还是低电平有效，转换 PWM 占空比。
static uint32_t apply_polarity(uint32_t duty)
{
#if BIRD_RGB_LED_ACTIVE_HIGH
    return duty;
#else
    return LED_PWM_MAX - duty;
#endif
}

// 写入单个 LEDC 通道的占空比。
static void set_channel(ledc_channel_t channel, uint32_t duty)
{
    duty = apply_polarity(duty);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
}

// 同时设置红、绿、蓝三个通道。
static void set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    set_channel(LEDC_CHANNEL_0, red);
    set_channel(LEDC_CHANNEL_1, green);
    set_channel(LEDC_CHANNEL_2, blue);
}

// 状态灯动画任务：充电、低电量、网络已连和初始化状态使用不同颜色。
static void status_led_task(void *arg)
{
    (void)arg;
    int level = 12;
    int direction = 1;

    while (1) {
        if (force_off) {
            set_rgb(0, 0, 0);
        } else if (charging) {
            set_rgb((uint8_t)level, (uint8_t)(level / 3), 0);
        } else if (low_battery) {
            set_rgb(255, 80, 0);
        } else if (battery_full || network_status == STATUS_LED_WIFI_CONNECTED) {
            set_rgb(0, 0, 255);
        } else {
            set_rgb(0, 0, (uint8_t)level);
        }

        level += direction * 8;
        if (level >= 255) {
            level = 255;
            direction = -1;
        } else if (level <= 12) {
            level = 12;
            direction = 1;
        }
        vTaskDelay(pdMS_TO_TICKS(35));
    }
}

// 初始化 LEDC 定时器和 RGB 三路 PWM 输出。
void status_led_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    const int pins[3] = {
        BIRD_RGB_LED_RED_PIN,
        BIRD_RGB_LED_GREEN_PIN,
        BIRD_RGB_LED_BLUE_PIN,
    };
    for (int i = 0; i < 3; i++) {
        ledc_channel_config_t channel = {
            .gpio_num = pins[i],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = (ledc_channel_t)i,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = apply_polarity(0),
            .hpoint = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&channel));
    }

    xTaskCreate(status_led_task, "status_led", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "ready, R=%d G=%d B=%d active_high=%d",
             BIRD_RGB_LED_RED_PIN,
             BIRD_RGB_LED_GREEN_PIN,
             BIRD_RGB_LED_BLUE_PIN,
             BIRD_RGB_LED_ACTIVE_HIGH);
}

// 更新网络状态，影响状态灯颜色。
void status_led_set_network(status_led_network_t status)
{
    network_status = status;
}

// 设置正在充电状态。
void status_led_set_charging(bool value)
{
    charging = value;
}

// 设置低电量状态。
void status_led_set_low_battery(bool value)
{
    low_battery = value;
}

// 设置满电状态。
void status_led_set_battery_full(bool value)
{
    battery_full = value;
}

// 强制关闭状态灯，长按休眠前使用。
void status_led_off(void)
{
    force_off = true;
    vTaskDelay(pdMS_TO_TICKS(50));
}

// 恢复状态灯自动显示。
void status_led_resume(void)
{
    force_off = false;
}
