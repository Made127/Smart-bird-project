#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdbool.h>

// 网络连接状态枚举，状态灯根据该状态显示初始化/断网/已联网。
typedef enum {
    STATUS_LED_INITIALIZING = 0,
    STATUS_LED_WIFI_DISCONNECTED,
    STATUS_LED_WIFI_CONNECTED,
} status_led_network_t;

// 初始化 RGB 状态灯。
void status_led_init(void);
// 设置网络状态。
void status_led_set_network(status_led_network_t status);
// 设置充电中状态。
void status_led_set_charging(bool charging);
// 设置低电量状态。
void status_led_set_low_battery(bool low_battery);
// 设置满电状态。
void status_led_set_battery_full(bool battery_full);
// 强制熄灭状态灯。
void status_led_off(void);
// 恢复状态灯显示。
void status_led_resume(void);

#endif
