#include "Button.h"

#include <inttypes.h>

#include "bird_test_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "Sound.h"
#include "StatusLED.h"

// 按键模块负责短按静音/取消静音，长按后进入软件休眠。
static const char *TAG = "Button";
static int button_active_level = BIRD_BUTTON_ACTIVE_LEVEL;

// 按当前有效电平判断按键是否处于按下状态。
static bool button_pressed(void)
{
    return gpio_get_level(BIRD_BUTTON_PIN) == button_active_level;
}

// 长按释放后进入 light sleep，并配置同一个按键作为唤醒源。
static void enter_software_power_off(void)
{
    gpio_int_type_t wake_type = button_active_level == 1
                                    ? GPIO_INTR_HIGH_LEVEL
                                    : GPIO_INTR_LOW_LEVEL;

    ESP_LOGI(TAG, "button released: enter light sleep, press again to wake");
    ESP_ERROR_CHECK(gpio_wakeup_enable(BIRD_BUTTON_PIN, wake_type));
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_err_t sleep_result = esp_light_sleep_start();
    if (sleep_result != ESP_OK) {
        ESP_LOGE(TAG, "light sleep failed: %s", esp_err_to_name(sleep_result));
        gpio_wakeup_disable(BIRD_BUTTON_PIN);
        status_led_resume();
        return;
    }

    gpio_wakeup_disable(BIRD_BUTTON_PIN);
    ESP_LOGI(TAG, "button wakeup detected, restarting");
    esp_restart();
}

// 按键扫描任务：包含启动释放防误触、消抖、短按和长按识别。
static void button_task(void *arg)
{
    (void)arg;
    bool stable_pressed = false;
    bool previous_raw = false;
    bool long_press_triggered = false;
    bool button_armed = false;
    TickType_t raw_changed_at = xTaskGetTickCount();
    TickType_t released_since = raw_changed_at;
    TickType_t pressed_at = 0;

    while (1) {
        bool raw_pressed = button_pressed();
        TickType_t now = xTaskGetTickCount();

        if (!button_armed) {
            if (raw_pressed) {
                released_since = now;
            } else if (now - released_since >=
                       pdMS_TO_TICKS(BIRD_BUTTON_ARM_RELEASE_MS)) {
                button_armed = true;
                previous_raw = false;
                stable_pressed = false;
                raw_changed_at = now;
                ESP_LOGI(TAG, "armed after stable release");
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (raw_pressed != previous_raw) {
            previous_raw = raw_pressed;
            raw_changed_at = now;
        }

        if (raw_pressed != stable_pressed &&
            now - raw_changed_at >= pdMS_TO_TICKS(BIRD_BUTTON_DEBOUNCE_MS)) {
            stable_pressed = raw_pressed;
            if (stable_pressed) {
                pressed_at = now;
                long_press_triggered = false;
                ESP_LOGI(TAG, "pressed, level=%d",
                         gpio_get_level(BIRD_BUTTON_PIN));
            } else {
                uint32_t held_ms =
                    (uint32_t)((now - pressed_at) * portTICK_PERIOD_MS);
                ESP_LOGI(TAG, "released, held=%" PRIu32 "ms", held_ms);
                if (long_press_triggered ||
                    held_ms >= BIRD_BUTTON_LONG_PRESS_MS) {
                    enter_software_power_off();
                } else if (held_ms >= BIRD_BUTTON_MIN_PRESS_MS) {
                    bool muted = !Sound_is_muted();
                    Sound_set_muted(muted);
                    ESP_LOGI(TAG, "short press: speaker %s",
                             muted ? "muted" : "unmuted");
                }
            }
        }

        if (stable_pressed && !long_press_triggered &&
            now - pressed_at >= pdMS_TO_TICKS(BIRD_BUTTON_LONG_PRESS_MS)) {
            long_press_triggered = true;
            ESP_LOGI(TAG, "long press detected, release to sleep");
            Sound_stop_immediate();
            status_led_off();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// 初始化按键 GPIO，并按配置决定是否自动识别按下电平。
void button_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << BIRD_BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));

    vTaskDelay(pdMS_TO_TICKS(BIRD_BUTTON_STARTUP_MS));
    int high_samples = 0;
    int sample_count = BIRD_BUTTON_IDLE_SAMPLE_MS / 10;
    for (int i = 0; i < sample_count; ++i) {
        high_samples += gpio_get_level(BIRD_BUTTON_PIN) == 1;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    int idle_level = high_samples > sample_count / 2 ? 1 : 0;
#if BIRD_BUTTON_AUTO_DETECT
    button_active_level = idle_level == 0 ? 1 : 0;
#endif

    xTaskCreate(button_task, "button", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG,
             "ready, pin=%d idle_level=%d (%d/%d high) press_level=%d "
             "long_press=%dms",
             BIRD_BUTTON_PIN,
             idle_level,
             high_samples,
             sample_count,
             button_active_level,
             BIRD_BUTTON_LONG_PRESS_MS);
}
