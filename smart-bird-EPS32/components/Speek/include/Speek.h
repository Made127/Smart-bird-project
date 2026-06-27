#ifndef SPEEK_H
#define SPEEK_H

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"

// I2S 麦克风接线：SCK=GPIO10，WS=GPIO11，SD=GPIO12。
#define I2S_SCK GPIO_NUM_10
#define I2S_WS  GPIO_NUM_11
#define I2S_SD  GPIO_NUM_12

// 初始化麦克风采集。
void speek_init(int sample_rate);
// 读取 I2S 原始采样。
size_t speek_read(int32_t *buffer, size_t samples);
// 获取当前麦克风音量。
int speek_get_volume(void);
// 执行一次语音检测和录音状态机。
void voice_task(void);

#endif
