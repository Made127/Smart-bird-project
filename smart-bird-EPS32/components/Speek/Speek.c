#include <stdlib.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "driver/i2s.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "Speek.h"
#include "AI.h"
#include "Sound.h"
#include "bird_test_config.h"

// 麦克风模块：I2S 采集音频、做简单音量 VAD，并把完整语音片段发给 AI。
#define I2S_RAW_FRAME 512
#define MONO_FRAME    (I2S_RAW_FRAME / 2)

static const char *TAG = "Speek";
static int32_t mic_raw[I2S_RAW_FRAME];
static int16_t frame_pcm[MONO_FRAME];
static int16_t *audio_cache = NULL;
static int cache_index = 0;
static int active_speech_samples = 0;
static int post_playback_mute_frames = 0;
static int silence_frames = 0;
static int speech_frames = 0;
static int barge_monitor_frames = 0;
static int barge_speech_frames = 0;
static int barge_echo_volume = 0;
static int barge_triggered = 0;
static TickType_t last_barge_check_tick = 0;

// 计算一帧 PCM 的平均绝对幅度，用作语音活动检测音量。
static int frame_volume(const int16_t *buf, int len)
{
    int64_t sum = 0;
    for (int i = 0; i < len; i++) {
        sum += abs(buf[i]);
    }
    return len > 0 ? (int)(sum / len) : 0;
}

// 把一帧 PCM 追加到录音缓存，集中做边界检查，避免长时间说话导致越界写入。
static void append_frame_to_cache(const int16_t *buf, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        // 这行很重要：任何录音路径写缓存前都必须检查上限，防止破坏堆内存。
        if (cache_index < BIRD_MAX_RECORD_SAMPLES) {
            audio_cache[cache_index++] = buf[i];
        }
    }
}

// 重置播放时插话检测状态。
static void reset_barge_monitor(void)
{
    barge_monitor_frames = 0;
    barge_speech_frames = 0;
    barge_echo_volume = 0;
    barge_triggered = 0;
}

// 发送录音缓存给服务器，播放结束后短暂屏蔽麦克风防止回声误触发。
static void send_audio_and_mute_mic(void)
{
    int samples = cache_index;
    cache_index = 0;
    active_speech_samples = 0;

    reset_barge_monitor();
    Sound_clear_interrupted();
    ai_send_audio(audio_cache, samples);

    if (barge_triggered) {
        post_playback_mute_frames = 0;
        silence_frames = 0;
        speech_frames = BIRD_SPEECH_START_FRAMES;
        ESP_LOGI(TAG, "user interrupted playback, continue recording");
        return;
    }

    i2s_zero_dma_buffer(I2S_NUM_1);
    post_playback_mute_frames = BIRD_POST_PLAYBACK_MUTE_FRAMES;
    ESP_LOGI(TAG, "ignore mic for %d frames after playback",
             post_playback_mute_frames);
}

// 将 I2S 32-bit 原始采样压缩为 16-bit PCM。
static int16_t mic_to_pcm16(int32_t raw)
{
    int32_t x = raw >> 14;
    if (x > 32767) {
        x = 32767;
    } else if (x < -32768) {
        x = -32768;
    }
    return (int16_t)x;
}

// 麦克风按左右声道槽位返回数据，这里把两个槽位平均成单声道 PCM。
static size_t stereo_slots_to_mono(const int32_t *raw, size_t raw_count,
                                   int16_t *mono)
{
    size_t mono_count = raw_count / 2;

    for (size_t i = 0; i < mono_count; i++) {
        int32_t left = mic_to_pcm16(raw[i * 2]);
        int32_t right = mic_to_pcm16(raw[i * 2 + 1]);
        mono[i] = (int16_t)((left + right) / 2);
    }

    return mono_count;
}

// 简单 VAD：平均音量超过阈值即认为正在说话。
static int is_speaking(const int16_t *buf, int len)
{
    return frame_volume(buf, len) > BIRD_VAD_THRESHOLD;
}

// 播放服务器回复时持续监听麦克风，检测用户是否插话打断。
static bool check_barge_in(void)
{
    TickType_t now = xTaskGetTickCount();
    if (last_barge_check_tick != 0 &&
        now - last_barge_check_tick > pdMS_TO_TICKS(120)) {
        reset_barge_monitor();
    }
    last_barge_check_tick = now;

    size_t bytes_read = 0;
    size_t latest_bytes = 0;
    esp_err_t err;

    do {
        bytes_read = 0;
        err = i2s_read(
            I2S_NUM_1,
            mic_raw,
            sizeof(mic_raw),
            &bytes_read,
            0);
        if (err == ESP_OK && bytes_read > 0) {
            latest_bytes = bytes_read;
        }
    } while (err == ESP_OK && bytes_read == sizeof(mic_raw));

    if (err != ESP_OK || latest_bytes < 2 * sizeof(int32_t)) {
        return false;
    }

    size_t raw_count = latest_bytes / sizeof(int32_t);
    size_t count = stereo_slots_to_mono(mic_raw, raw_count, frame_pcm);
    int volume = frame_volume(frame_pcm, (int)count);
    barge_monitor_frames++;

    if (barge_monitor_frames <= BIRD_BARGE_IN_ECHO_CALIBRATION_FRAMES) {
        if (volume > barge_echo_volume) {
            barge_echo_volume = volume;
        }
        return false;
    }

    int adaptive_threshold = barge_echo_volume + (barge_echo_volume / 2);
    if (adaptive_threshold < BIRD_BARGE_IN_MIN_VOLUME) {
        adaptive_threshold = BIRD_BARGE_IN_MIN_VOLUME;
    }

    if (volume > adaptive_threshold) {
        barge_speech_frames++;
    } else {
        barge_speech_frames = 0;
        if (volume > barge_echo_volume) {
            barge_echo_volume =
                (barge_echo_volume * 7 + volume) / 8;
        }
    }

    if (barge_speech_frames < BIRD_BARGE_IN_START_FRAMES) {
        return false;
    }

    append_frame_to_cache(frame_pcm, count);
    active_speech_samples += (int)count;
    barge_triggered = 1;
    ESP_LOGI(TAG, "barge-in detected, volume=%d threshold=%d",
             volume, adaptive_threshold);
    return true;
}

// 初始化麦克风 I2S 输入，并注册播放打断检测回调。
void speek_init(int sample_rate)
{
    // 这行很重要：录音缓存按最大录音时长申请，申请失败后不能继续录音。
    audio_cache = (int16_t *)heap_caps_malloc(
        BIRD_MAX_RECORD_SAMPLES * sizeof(int16_t), MALLOC_CAP_8BIT);
    if (!audio_cache) {
        ESP_LOGE(TAG, "audio cache malloc failed");
        return;
    }

    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_RX,
        .sample_rate = sample_rate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = 0,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SCK,
        .ws_io_num = I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_SD
    };

    ESP_ERROR_CHECK(i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL));
    ESP_ERROR_CHECK(i2s_set_pin(I2S_NUM_1, &pin_config));
    Sound_set_interrupt_check(check_barge_in);

    ESP_LOGI(TAG, "mic ready, rate=%d, sck=%d, ws=%d, sd=%d",
             sample_rate, I2S_SCK, I2S_WS, I2S_SD);
}

// 阻塞读取指定数量的 I2S 原始采样。
size_t speek_read(int32_t *buffer, size_t samples)
{
    size_t bytes_read = 0;
    i2s_read(I2S_NUM_1, buffer, samples * sizeof(int32_t),
             &bytes_read, portMAX_DELAY);
    return bytes_read / sizeof(int32_t);
}

// 读取一帧音频并返回当前音量，便于调试 VAD 阈值。
int speek_get_volume(void)
{
    size_t raw_count = speek_read(mic_raw, I2S_RAW_FRAME);
    size_t count = stereo_slots_to_mono(mic_raw, raw_count, frame_pcm);
    int64_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum += abs(frame_pcm[i]);
    }
    return count > 0 ? (int)(sum / count) : 0;
}

// 主循环中的语音检测任务：累计语音片段，静音结束后上传给服务器。
void voice_task(void)
{
    static int external_playback_seen = 0;

    if (!audio_cache) {
        return;
    }

    if (Sound_is_playing()) {
        external_playback_seen = 1;
        if (!barge_triggered) {
            cache_index = 0;
            active_speech_samples = 0;
            silence_frames = 0;
            speech_frames = 0;
        }
        return;
    }

    if (external_playback_seen) {
        external_playback_seen = 0;
        if (barge_triggered) {
            silence_frames = 0;
            speech_frames = BIRD_SPEECH_START_FRAMES;
            ESP_LOGI(TAG, "continue recording app playback interruption");
        } else {
            i2s_zero_dma_buffer(I2S_NUM_1);
            post_playback_mute_frames =
                BIRD_REMOTE_POST_PLAYBACK_MUTE_FRAMES;
        }
    }

    size_t raw_count = speek_read(mic_raw, I2S_RAW_FRAME);
    if (raw_count < 2) {
        return;
    }

    if (post_playback_mute_frames > 0) {
        post_playback_mute_frames--;
        cache_index = 0;
        active_speech_samples = 0;
        silence_frames = 0;
        speech_frames = 0;
        return;
    }

    size_t count = stereo_slots_to_mono(mic_raw, raw_count, frame_pcm);

    if (is_speaking(frame_pcm, (int)count)) {
        silence_frames = 0;
        speech_frames++;
        append_frame_to_cache(frame_pcm, count);
        active_speech_samples += (int)count;

        if (speech_frames < BIRD_SPEECH_START_FRAMES) {
            return;
        }

        if (cache_index >= BIRD_MAX_RECORD_SAMPLES) {
            ESP_LOGW(TAG, "record full, send len=%d voiced=%d",
                     cache_index, active_speech_samples);
            send_audio_and_mute_mic();
            speech_frames = 0;
        }
    } else {
        if (cache_index > 0 && speech_frames >= BIRD_SPEECH_START_FRAMES) {
            // 确认开始说话后，静音帧也要保留；ASR 需要连续语音片段，而不是被阈值切碎的片段。
            append_frame_to_cache(frame_pcm, count);
        }

        silence_frames++;

        if (silence_frames > BIRD_SILENCE_FRAMES &&
            active_speech_samples > BIRD_MIN_SPEECH_SAMPLES) {
            ESP_LOGI(TAG, "send to AI len=%d voiced=%d",
                     cache_index, active_speech_samples);
            send_audio_and_mute_mic();
            silence_frames = 0;
            speech_frames = 0;
        }

        if (silence_frames > BIRD_SILENCE_FRAMES && cache_index > 0) {
            cache_index = 0;
            active_speech_samples = 0;
            speech_frames = 0;
        }

        if (silence_frames > BIRD_SILENCE_FRAMES && cache_index == 0) {
            speech_frames = 0;
        }
    }
}
