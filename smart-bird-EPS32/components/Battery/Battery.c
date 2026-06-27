#include "Battery.h"

#include <string.h>

#include "StatusLED.h"
#include "bird_test_config.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 电池模块通过 ADC 分压读取单节锂电电压，并换算成百分比和状态灯提示。
static const char *TAG = "Battery";
static adc_oneshot_unit_handle_t adc_handle;
static adc_cali_handle_t cali_handle;
static bool cali_ready;
static bool initialized;
static battery_status_t latest_status;
static battery_status_t last_valid_status;
static bool has_last_valid_status;

// 限制百分比范围，避免电压换算后出现小于 0 或大于 100 的结果。
static int clamp_percent(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 100) {
        return 100;
    }
    return value;
}

// 根据锂电池典型放电曲线做分段线性插值，得到估算电量百分比。
static int interpolate_percent(int voltage_mv)
{
    typedef struct {
        int mv;
        int percent;
    } point_t;

    static const point_t curve[] = {
        {3300, 0},
        {3450, 5},
        {3600, 15},
        {3700, 30},
        {3800, 50},
        {3900, 65},
        {4000, 80},
        {4100, 92},
        {4200, 100},
    };

    if (voltage_mv <= curve[0].mv) {
        return curve[0].percent;
    }
    for (int i = 1; i < (int)(sizeof(curve) / sizeof(curve[0])); i++) {
        if (voltage_mv <= curve[i].mv) {
            int mv_span = curve[i].mv - curve[i - 1].mv;
            int pct_span = curve[i].percent - curve[i - 1].percent;
            return curve[i - 1].percent +
                   ((voltage_mv - curve[i - 1].mv) * pct_span) / mv_span;
        }
    }
    return 100;
}

// 读取可选充电/满电状态脚；未接引脚时返回 false 表示无该状态。
static bool read_optional_level(gpio_num_t pin, bool *value)
{
    if (pin == GPIO_NUM_NC) {
        return false;
    }
    *value = gpio_get_level(pin) == BIRD_BATTERY_STATUS_ACTIVE_LEVEL;
    return true;
}

// 配置可选充电状态输入脚，默认启用上拉以适配常见低有效充电模块。
static void configure_optional_status_pin(gpio_num_t pin)
{
    if (pin == GPIO_NUM_NC) {
        return;
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
}

// 读取 ADC 原始值并转换为分压点电压，优先使用 ESP-IDF 校准结果。
static esp_err_t read_adc_voltage_mv(int *adc_mv_out)
{
    int raw = 0;
    esp_err_t err = adc_oneshot_read(adc_handle, BIRD_BATTERY_ADC_CHANNEL, &raw);
    if (err != ESP_OK) {
        return err;
    }

    int adc_mv = 0;
    if (cali_ready) {
        err = adc_cali_raw_to_voltage(cali_handle, raw, &adc_mv);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        adc_mv = (raw * 3100) / 4095;
    }

    *adc_mv_out = adc_mv;
    return ESP_OK;
}

// 连续采样 5 次后去掉最高/最低值，用中间 3 次平均值计算真实电池电压。
static esp_err_t read_battery_once(battery_status_t *status)
{
#if BIRD_BATTERY_ENABLED
    int samples[5] = {0};
    for (int i = 0; i < 5; i++) {
        esp_err_t err = read_adc_voltage_mv(&samples[i]);
        if (err != ESP_OK) {
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    for (int i = 0; i < 4; i++) {
        for (int j = i + 1; j < 5; j++) {
            if (samples[j] < samples[i]) {
                int tmp = samples[i];
                samples[i] = samples[j];
                samples[j] = tmp;
            }
        }
    }

    int adc_mv = (samples[1] + samples[2] + samples[3]) / 3;

    // 这行很重要：把 ADC 分压点电压还原成电池真实电压，电阻配置必须准确。
    int voltage_mv =
        (adc_mv * (BIRD_BATTERY_DIVIDER_TOP_OHM + BIRD_BATTERY_DIVIDER_BOTTOM_OHM)) /
        BIRD_BATTERY_DIVIDER_BOTTOM_OHM;

    // 这行很重要：超出单节锂电合理范围时拒绝数据，避免接线错误造成错误电量。
    if (voltage_mv < BIRD_BATTERY_MIN_VALID_MV ||
        voltage_mv > BIRD_BATTERY_MAX_VALID_MV) {
        ESP_LOGW(TAG, "reject invalid voltage=%dmV adc=%dmV, check divider/GND",
                 voltage_mv,
                 adc_mv);
        if (has_last_valid_status) {
            *status = last_valid_status;
            return ESP_OK;
        }
        return ESP_ERR_INVALID_RESPONSE;
    }

    int percent = clamp_percent(interpolate_percent(voltage_mv));

    bool charging = false;
    bool full_pin = false;
    bool has_charging = read_optional_level(BIRD_BATTERY_CHG_PIN, &charging);
    bool has_full = read_optional_level(BIRD_BATTERY_FULL_PIN, &full_pin);

    status->valid = true;
    status->voltage_mv = voltage_mv;
    status->percent = percent;
    status->low = percent <= BIRD_BATTERY_LOW_PERCENT;
    status->charging = has_charging ? charging : false;
    status->full = has_full ? full_pin : voltage_mv >= BIRD_BATTERY_FULL_MV;
    last_valid_status = *status;
    has_last_valid_status = true;
    return ESP_OK;
#else
    (void)status;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

// 周期性刷新电池状态，并把低电量/充电/满电状态同步到状态灯模块。
static void battery_task(void *arg)
{
    (void)arg;
    while (1) {
        battery_status_t status = {0};
        esp_err_t err = read_battery_once(&status);
        if (err == ESP_OK) {
            latest_status = status;
            status_led_set_low_battery(status.low);
            status_led_set_charging(status.charging);
            status_led_set_battery_full(status.full);
            ESP_LOGI(TAG, "voltage=%dmV percent=%d%% charging=%d full=%d low=%d",
                     status.voltage_mv,
                     status.percent,
                     status.charging,
                     status.full,
                     status.low);
        } else {
            latest_status.valid = false;
            ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// 初始化 ADC、电池状态脚和电池后台监测任务。
void Battery_init(void)
{
#if BIRD_BATTERY_ENABLED
    if (initialized) {
        return;
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BIRD_BATTERY_ADC_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &adc_handle));

    // 11dB 衰减用于扩大 ADC 可测范围，适合读取分压后的锂电电压。
    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_11,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(
        adc_handle,
        BIRD_BATTERY_ADC_CHANNEL,
        &chan_cfg));

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BIRD_BATTERY_ADC_UNIT,
        .atten = ADC_ATTEN_DB_11,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    cali_ready =
        adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_handle) == ESP_OK;

    configure_optional_status_pin(BIRD_BATTERY_CHG_PIN);
    configure_optional_status_pin(BIRD_BATTERY_FULL_PIN);

    initialized = true;
    ESP_LOGI(TAG, "ready, adc_gpio=%d capacity=%dmAh divider=%d/%d cali=%d",
             BIRD_BATTERY_ADC_GPIO,
             BIRD_BATTERY_CAPACITY_MAH,
             BIRD_BATTERY_DIVIDER_TOP_OHM,
             BIRD_BATTERY_DIVIDER_BOTTOM_OHM,
             cali_ready);
    xTaskCreate(battery_task, "battery", 3072, NULL, 3, NULL);
#endif
}

// 获取最近一次有效电池状态；无有效数据时返回 false。
bool Battery_get_status(battery_status_t *status)
{
    if (!status || !latest_status.valid) {
        return false;
    }
    memcpy(status, &latest_status, sizeof(*status));
    return true;
}
