#ifndef SOUND_H
#define SOUND_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"

// I2S 喇叭/功放接线：BCLK=GPIO15，LRC/WS=GPIO16，DIN=GPIO17。
#define I2S_BCLK_PIN GPIO_NUM_15
#define I2S_LRC_PIN  GPIO_NUM_16
#define I2S_DOUT_PIN GPIO_NUM_17

// 播放过程中用于判断是否需要被用户语音打断的回调类型。
typedef bool (*sound_interrupt_check_t)(void);

// 初始化喇叭输出。
void Sound_init(int sample_rate);
// 设置播放打断检测回调。
void Sound_set_interrupt_check(sound_interrupt_check_t check);
// 清除播放打断标记。
void Sound_clear_interrupted(void);
// 查询是否发生过播放打断。
bool Sound_was_interrupted(void);
// 标记播放开始。
void Sound_begin_playback(void);
// 标记播放结束。
void Sound_end_playback(void);
// 查询当前是否正在播放。
bool Sound_is_playing(void);
// 设置静音状态。
void Sound_set_muted(bool muted);
// 查询静音状态。
bool Sound_is_muted(void);
// 播放单声道 PCM。
void Sound_play_pcm_mono(const int16_t *buffer, size_t samples);
// 播放开机测试音。
void Sound_play_test_tone(void);
// 正常停止播放。
void Sound_stop(void);
// 立即停止播放。
void Sound_stop_immediate(void);

#endif
