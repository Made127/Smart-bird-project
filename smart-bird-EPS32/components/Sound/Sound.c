#include <stdio.h>
#include <math.h>

#include "driver/i2s.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "Sound.h"
#include "bird_test_config.h"

// 喇叭播放模块：通过 I2S 输出 PCM，并支持静音和播放期间插话中断。
static const char *TAG = "Sound";
static int16_t stereo_buffer[512 * 2];
static int16_t test_tone_buffer[256];
static sound_interrupt_check_t interrupt_check;
static volatile bool playback_interrupted;
static volatile bool playback_active;
static volatile bool speaker_muted;

// 初始化 I2S0 作为喇叭输出。
void Sound_init(int sample_rate)
{
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = sample_rate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = 0,
        .dma_buf_count = 8,
        .dma_buf_len = 512,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCLK_PIN,
        .ws_io_num = I2S_LRC_PIN,
        .data_out_num = I2S_DOUT_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_0, &pin_config));
    i2s_zero_dma_buffer(I2S_NUM_0);

    ESP_LOGI(TAG, "speaker ready, rate=%d, bclk=%d, lrc=%d, dout=%d",
             sample_rate, I2S_BCLK_PIN, I2S_LRC_PIN, I2S_DOUT_PIN);
}

// 注册播放中断检测函数，由麦克风模块提供插话检测。
void Sound_set_interrupt_check(sound_interrupt_check_t check)
{
    interrupt_check = check;
}

// 清除上一轮播放的中断标记。
void Sound_clear_interrupted(void)
{
    playback_interrupted = false;
}

// 查询上一轮播放是否被用户插话打断。
bool Sound_was_interrupted(void)
{
    return playback_interrupted;
}

// 标记播放开始，麦克风任务会据此暂停普通录音。
void Sound_begin_playback(void)
{
    playback_active = true;
}

// 标记播放结束。
void Sound_end_playback(void)
{
    playback_active = false;
}

// 查询当前是否处于播放状态。
bool Sound_is_playing(void)
{
    return playback_active;
}

// 设置喇叭静音；静音时立即停止当前输出。
void Sound_set_muted(bool muted)
{
    speaker_muted = muted;
    if (muted) {
        Sound_stop_immediate();
    }
    ESP_LOGI(TAG, "speaker %s", muted ? "muted" : "unmuted");
}

// 查询喇叭是否静音。
bool Sound_is_muted(void)
{
    return speaker_muted;
}

// 播放单声道 16-bit PCM；写入 I2S 前复制成左右双声道并按音量缩放。
void Sound_play_pcm_mono(const int16_t *buffer, size_t samples)
{
    if (speaker_muted) {
        return;
    }

    size_t index = 0;
    size_t total_written = 0;

    while (index < samples) {
        // 这行很重要：播放过程中持续检查用户是否插话，检测到后立即停止回复音频。
        if (interrupt_check && interrupt_check()) {
            playback_interrupted = true;
            Sound_stop_immediate();
            ESP_LOGI(TAG, "playback interrupted by user speech");
            break;
        }

        size_t n = samples - index;
        if (n > 128) {
            n = 128;
        }

        for (size_t i = 0; i < n; i++) {
            int32_t scaled = ((int32_t)buffer[index + i] * BIRD_SPEAKER_VOLUME_PERCENT) / 100;
            int16_t s = (int16_t)scaled;
            stereo_buffer[i * 2] = s;
            stereo_buffer[i * 2 + 1] = s;
        }

        size_t bytes_written = 0;
        esp_err_t err = i2s_write(
            I2S_NUM_0,
            stereo_buffer,
            n * 2 * sizeof(int16_t),
            &bytes_written,
            portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2s write failed: %s", esp_err_to_name(err));
            break;
        }
        total_written += bytes_written;
        index += n;
    }

    (void)total_written;
}

// 播放 1 秒开机测试音，方便确认喇叭接线和功放工作正常。
void Sound_play_test_tone(void)
{
    size_t generated = 0;

    ESP_LOGI(TAG, "play 1 second startup test tone");

    while (generated < 16000) {
        if (speaker_muted) {
            break;
        }
        size_t count = 16000 - generated;
        if (count > 256) {
            count = 256;
        }

        for (size_t i = 0; i < count; i++) {
            size_t sample_index = generated + i;
            float frequency = sample_index < 8000 ? 660.0f : 880.0f;
            float phase = 2.0f * 3.1415926f * frequency *
                          (float)sample_index / 16000.0f;
            test_tone_buffer[i] = (int16_t)(28000.0f * sinf(phase));
        }

        Sound_play_pcm_mono(test_tone_buffer, count);
        generated += count;
    }

    Sound_stop();
    ESP_LOGI(TAG, "startup test tone complete");
}

// 正常停止播放：等待 DMA 缓冲区播完后再清空。
void Sound_stop(void)
{
    if (playback_interrupted) {
        Sound_stop_immediate();
        return;
    }

    // 等最后的 DMA 缓冲区送到功放后再清空，避免结尾被截断。
    vTaskDelay(pdMS_TO_TICKS(350));
    i2s_zero_dma_buffer(I2S_NUM_0);
}

// 立即停止播放并清空 DMA，通常用于静音或插话打断。
void Sound_stop_immediate(void)
{
    i2s_stop(I2S_NUM_0);
    i2s_zero_dma_buffer(I2S_NUM_0);
    i2s_start(I2S_NUM_0);
}
