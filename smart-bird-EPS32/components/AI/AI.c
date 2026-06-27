#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "AI.h"
#include "Battery.h"
#include "IR.h"
#include "LS.h"
#include "Sound.h"
#include "SoilProbe.h"
#include "bird_test_config.h"

// AI 模块负责 ESP32 与本地 Flask 服务之间的音频上传、回复播放和传感器上报。
#define HTTP_TIMEOUT_MS 120000
#define COMMAND_POLL_MS 1000

static const char *TAG = "AI";
static char server_base_url[128] = {0};
static char upload_url[160] = {0};
static char pending_url[160] = {0};
static char command_url[160] = {0};
static char collect_url[160] = {0};
static SemaphoreHandle_t ai_request_mutex;

// 将当前红外、光照、土壤和电池数据放入 HTTP 头，服务器据此更新设备状态。
static void ai_set_sensor_headers(esp_http_client_handle_t client)
{
    char ir_value[8];
    char lux_x10_value[16];
    char soil_value[16];
    char battery_value[16];
    int ir_raw = IR_read();
    float lux = LS_Get_Lux();
    int lux_x10 = lux >= 0.0f ? (int)(lux * 10.0f + 0.5f) : -1;
    soil_probe_data_t soil;
    battery_status_t battery;

    snprintf(ir_value, sizeof(ir_value), "%d", ir_raw);
    snprintf(lux_x10_value, sizeof(lux_x10_value), "%d", lux_x10);
    esp_http_client_set_header(client, "X-IR-Raw", ir_value);
    esp_http_client_set_header(client, "X-Light-Lux-X10", lux_x10_value);

    // 这行很重要：服务器依赖这些 HTTP 头同步电池状态，字段名要和 app.py 保持一致。
    if (Battery_get_status(&battery)) {
        snprintf(battery_value, sizeof(battery_value), "%d", battery.voltage_mv);
        esp_http_client_set_header(client, "X-Battery-Voltage-Mv", battery_value);
        snprintf(battery_value, sizeof(battery_value), "%d", battery.percent);
        esp_http_client_set_header(client, "X-Battery-Percent", battery_value);
        snprintf(battery_value, sizeof(battery_value), "%d", battery.low ? 1 : 0);
        esp_http_client_set_header(client, "X-Battery-Low", battery_value);
        snprintf(battery_value, sizeof(battery_value), "%d", battery.charging ? 1 : 0);
        esp_http_client_set_header(client, "X-Battery-Charging", battery_value);
        snprintf(battery_value, sizeof(battery_value), "%d", battery.full ? 1 : 0);
        esp_http_client_set_header(client, "X-Battery-Full", battery_value);
    }

    // 这行很重要：土壤数据只在最近一次读取有效时上报，避免把无效值写进历史记录。
    if (SoilProbe_get_latest(&soil)) {
        snprintf(soil_value, sizeof(soil_value), "%d", soil.temperature_x10);
        esp_http_client_set_header(client, "X-Soil-Temp-X10", soil_value);
        snprintf(soil_value, sizeof(soil_value), "%u", soil.humidity_x10);
        esp_http_client_set_header(client, "X-Soil-Humidity-X10", soil_value);
        snprintf(soil_value, sizeof(soil_value), "%u", soil.ec_us_cm);
        esp_http_client_set_header(client, "X-Soil-Ec", soil_value);
        snprintf(soil_value, sizeof(soil_value), "%u", soil.salt_ppm);
        esp_http_client_set_header(client, "X-Soil-Salt", soil_value);
        snprintf(soil_value, sizeof(soil_value), "%u", soil.nitrogen_mg_kg);
        esp_http_client_set_header(client, "X-Soil-N", soil_value);
        snprintf(soil_value, sizeof(soil_value), "%u", soil.phosphorus_mg_kg);
        esp_http_client_set_header(client, "X-Soil-P", soil_value);
        snprintf(soil_value, sizeof(soil_value), "%u", soil.potassium_mg_kg);
        esp_http_client_set_header(client, "X-Soil-K", soil_value);
        snprintf(soil_value, sizeof(soil_value), "%u", soil.ph_x10);
        esp_http_client_set_header(client, "X-Soil-Ph-X10", soil_value);
    }
}

// 从 HTTP 响应流读取 PCM 数据并边下载边播放，支持用户插话打断播放。
static int ai_read_and_play_pcm(esp_http_client_handle_t client,
                                int expected_bytes)
{
    uint8_t net_buf[1024];
    uint8_t last_byte = 0;
    int has_last = 0;
    int total_bytes = 0;
    int received_bytes = 0;
    int empty_reads = 0;

    while (expected_bytes < 0 || received_bytes < expected_bytes) {
        int read_len = esp_http_client_read(client, (char *)net_buf, sizeof(net_buf));
        if (read_len < 0) {
            ESP_LOGE(TAG, "read failed");
            break;
        }
        if (read_len == 0) {
            empty_reads++;
            if (empty_reads >= 250) {
                ESP_LOGE(TAG, "audio read timeout, received=%d expected=%d",
                         received_bytes, expected_bytes);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        empty_reads = 0;
        received_bytes += read_len;

        int offset = 0;
        int16_t pcm[512];
        int sample_count = 0;

        if (has_last && read_len > 0) {
            pcm[sample_count++] = (int16_t)((uint16_t)last_byte | ((uint16_t)net_buf[0] << 8));
            offset = 1;
            has_last = 0;
        }

        for (int i = offset; i + 1 < read_len && sample_count < 512; i += 2) {
            uint16_t lo = net_buf[i];
            uint16_t hi = net_buf[i + 1];
            pcm[sample_count++] = (int16_t)(lo | (hi << 8));
        }

        if (((read_len - offset) & 1) != 0) {
            last_byte = net_buf[read_len - 1];
            has_last = 1;
        }

        if (sample_count > 0) {
            Sound_play_pcm_mono(pcm, sample_count);
            total_bytes += sample_count * 2;
            if (Sound_was_interrupted()) {
                ESP_LOGI(TAG, "stop response download after barge-in");
                break;
            }
        }
    }

    Sound_stop();
    if (Sound_was_interrupted()) {
        return 1;
    }
    ESP_LOGI(TAG, "played %d bytes, received %d/%d",
             total_bytes, received_bytes, expected_bytes);
    return total_bytes > 0 &&
           (expected_bytes < 0 || received_bytes >= expected_bytes) ? 0 : -1;
}

// 保存服务器地址并拼出所有设备端需要访问的 API 地址。
void ai_init(const char *server_ip, int server_port)
{
    snprintf(server_base_url, sizeof(server_base_url),
             "http://%s:%d", server_ip, server_port);
    snprintf(upload_url, sizeof(upload_url), "%s/upload", server_base_url);
    snprintf(pending_url, sizeof(pending_url), "%s/device/pending", server_base_url);
    snprintf(command_url, sizeof(command_url), "%s/device/command", server_base_url);
    snprintf(collect_url, sizeof(collect_url), "%s/device/collect", server_base_url);
    // 这行很重要：所有 HTTP 请求共用一个通道，互斥锁避免上传、轮询和采集互相打断。
    ai_request_mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "server=%s", upload_url);
}

// 在已经拿到互斥锁的情况下采集一次传感器，并把结果 POST 给服务器。
static int ai_collect_now_unlocked(const char *source)
{
    esp_err_t probe_err = SoilProbe_read_once();
    if (probe_err != ESP_OK) {
        ESP_LOGW(TAG, "soil probe read skipped/failed: %s", esp_err_to_name(probe_err));
    }

    esp_http_client_config_t config = {
        .url = collect_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
        .buffer_size = 512
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "collect http init failed");
        return -1;
    }

    ai_set_sensor_headers(client);
    esp_http_client_set_header(client, "X-Collect-Source", source ? source : "device");
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "collect open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return -2;
    }

    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "collect source=%s status=%d len=%d",
             source ? source : "device", status, content_length);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status == 200 ? 0 : -3;
}

// 对外暴露的立即采集接口，用互斥锁避免与语音上传/拉取播放同时占用 HTTP 通道。
int ai_collect_now(const char *source)
{
    // 这行很重要：采集接口也要抢同一把锁，避免和语音上传同时占用 HTTP 客户端。
    if (!ai_request_mutex ||
        xSemaphoreTake(ai_request_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "collect skipped, AI channel busy");
        return -1;
    }
    int result = ai_collect_now_unlocked(source);
    xSemaphoreGive(ai_request_mutex);
    return result;
}

// 拉取手机 App 下发的命令，目前主要用于远程触发一次传感器采集。
static void ai_handle_command_unlocked(void)
{
    esp_http_client_config_t config = {
        .url = command_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 2500,
        .buffer_size = 256
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return;
    }

    ai_set_sensor_headers(client);
    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        (void)esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            char command[64] = {0};
            int read_len = esp_http_client_read(client, command, sizeof(command) - 1);
            if (read_len > 0) {
                command[read_len] = '\0';
                ESP_LOGI(TAG, "server command=%s", command);
                if (strncmp(command, "collect", 7) == 0) {
                    const char *source = strchr(command, ':');
                    ai_collect_now_unlocked(source ? source + 1 : "remote");
                }
            }
        } else if (status != 204) {
            ESP_LOGW(TAG, "command status=%d", status);
        }
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
}

// 后台任务：轮询服务器是否有待播放音频，同时定期检查远程命令。
static void ai_remote_audio_task(void *arg)
{
    (void)arg;
    TickType_t last_command_poll = 0;

    while (1) {
        if (ai_request_mutex &&
            xSemaphoreTake(ai_request_mutex, 0) == pdTRUE) {
            TickType_t now = xTaskGetTickCount();
            if (now - last_command_poll >= pdMS_TO_TICKS(COMMAND_POLL_MS)) {
                last_command_poll = now;
                ai_handle_command_unlocked();
            }

            esp_http_client_config_t config = {
                .url = pending_url,
                .method = HTTP_METHOD_GET,
                .timeout_ms = 2500,
                .buffer_size = 1024
            };
            esp_http_client_handle_t client = esp_http_client_init(&config);
            if (client) {
                ai_set_sensor_headers(client);
                esp_err_t err = esp_http_client_open(client, 0);
                if (err == ESP_OK) {
                    int content_length = esp_http_client_fetch_headers(client);
                    int status = esp_http_client_get_status_code(client);
                    if (status == 200 && content_length > 0) {
                        ESP_LOGI(TAG, "app audio received, bytes=%d", content_length);
                        Sound_clear_interrupted();
                        Sound_begin_playback();
                        ai_read_and_play_pcm(client, content_length);
                        Sound_end_playback();
                    } else if (status != 204 && status != 200) {
                        ESP_LOGW(TAG, "pending audio status=%d", status);
                    }
                    esp_http_client_close(client);
                }
                esp_http_client_cleanup(client);
            }
            xSemaphoreGive(ai_request_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// 后台任务：开机采集一次，之后按配置间隔自动采集。
static void ai_collect_schedule_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(5000));
    ai_collect_now("boot");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(BIRD_COLLECT_INTERVAL_SECONDS * 1000));
        ai_collect_now("scheduled");
    }
}

// 启动远程音频轮询任务和定时采集任务。
void ai_start_remote_audio(void)
{
    xTaskCreate(
        ai_remote_audio_task,
        "app_audio",
        6144,
        NULL,
        4,
        NULL);
    xTaskCreate(
        ai_collect_schedule_task,
        "collect_sched",
        4096,
        NULL,
        3,
        NULL);
}

// 上传麦克风录到的一段 PCM 音频，并播放服务器返回的 PCM 回复。
int ai_send_audio(int16_t *audio, int len)
{
    if (!audio || len <= 0) {
        return -1;
    }

    // 这行很重要：语音上传期间不能同时轮询播放音频，否则会造成 I2S/HTTP 状态混乱。
    if (!ai_request_mutex ||
        xSemaphoreTake(ai_request_mutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "AI channel busy");
        return -6;
    }

    ESP_LOGI(TAG, "upload start, samples=%d, bytes=%d, url=%s",
             len, len * (int)sizeof(int16_t), upload_url);

    esp_http_client_config_t config = {
        .url = upload_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 1024
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "http init failed");
        xSemaphoreGive(ai_request_mutex);
        return -2;
    }

    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
    esp_http_client_set_header(client, "X-Audio-Format", "pcm_s16le");
    esp_http_client_set_header(client, "X-Sample-Rate", "16000");
    esp_http_client_set_header(client, "X-Channels", "1");

    ai_set_sensor_headers(client);

    int body_len = len * sizeof(int16_t);
    ESP_LOGI(TAG, "http open...");
    esp_err_t err = esp_http_client_open(client, body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        xSemaphoreGive(ai_request_mutex);
        return -3;
    }

    ESP_LOGI(TAG, "http write...");
    int written = esp_http_client_write(client, (const char *)audio, body_len);
    if (written != body_len) {
        ESP_LOGE(TAG, "upload failed, written=%d/%d", written, body_len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        xSemaphoreGive(ai_request_mutex);
        return -4;
    }

    ESP_LOGI(TAG, "wait response...");
    int content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "response headers status=%d content_length=%d",
             status, content_length);

    if (status != 200) {
        ESP_LOGE(TAG, "bad response status=%d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        xSemaphoreGive(ai_request_mutex);
        return -5;
    }

    ESP_LOGI(TAG, "play response pcm...");
    Sound_begin_playback();
    int result = ai_read_and_play_pcm(client, content_length);
    Sound_end_playback();

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    xSemaphoreGive(ai_request_mutex);
    return result;
}
