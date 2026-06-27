#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "BSP.h"

// ESP32 主入口：按硬件依赖顺序初始化外设、网络和语音 AI 任务。
void app_main(void)
{
    printf("[BIRD_TEST] boot\n");

    // 状态灯和电池最先启动，便于开机阶段提示网络和电量状态。
    status_led_init();
    status_led_set_network(STATUS_LED_INITIALIZING);
    Battery_init();

    // 初始化喇叭、按键和开机测试音；按键可控制静音/休眠。
    Sound_init(BIRD_SAMPLE_RATE);
    button_init();
    Sound_play_test_tone();

    // 初始化本地传感器，后续请求服务器时会随 HTTP 头一并上报。
    IR_init();
    LS_init();
    SoilProbe_init();

    // WiFi 连接失败时停在错误提示循环，避免后续 AI 请求无目标服务器。
    if (wifi_connect(BIRD_WIFI_SSID, BIRD_WIFI_PASSWORD) != 0) {
        while (1) {
            printf("[BIRD_TEST] WiFi failed. Check BIRD_WIFI_SSID/BIRD_WIFI_PASSWORD, then reset.\n");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
    status_led_set_network(STATUS_LED_WIFI_CONNECTED);

    speek_init(BIRD_SAMPLE_RATE);
    ai_init(BIRD_SERVER_IP, BIRD_SERVER_PORT);
    ai_start_remote_audio();

    // 主循环持续做语音活动检测，检测到完整一句话后上传服务器处理。
    while (1) {
        voice_task();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
