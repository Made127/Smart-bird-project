#ifndef IR_H
#define IR_H

#include "driver/gpio.h"
#include "esp_err.h"

// 红外人体感应模块 OUT 接到 GPIO38。
#define IR_PIN GPIO_NUM_38

// 初始化红外输入脚。
esp_err_t IR_init(void);
// 读取原始电平。
int IR_read(void);
// 返回是否检测到人。
int IR_has_person(void);

#endif
