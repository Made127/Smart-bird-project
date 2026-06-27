#ifndef LS_H
#define LS_H

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"

// BH1750 使用 I2C0，总线接线为 SDA=GPIO7、SCL=GPIO6。
#define I2C_Port I2C_NUM_0
#define BH1750_ADDR 0x23
#define BH1750_SDA GPIO_NUM_7
#define BH1750_SCL GPIO_NUM_6

// 初始化光照传感器。
esp_err_t LS_init(void);
// 获取 lux 光照值。
float LS_Get_Lux(void);

#endif
