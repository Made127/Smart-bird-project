#ifndef BATTERY_H
#define BATTERY_H

#include <stdbool.h>

// 电池状态快照，供 AI 上报和状态灯显示使用。
typedef struct {
    bool valid;
    int voltage_mv;
    int percent;
    bool low;
    bool charging;
    bool full;
} battery_status_t;

// 启动电池检测任务。
void Battery_init(void);
// 读取最近一次有效的电池状态。
bool Battery_get_status(battery_status_t *status);

#endif
