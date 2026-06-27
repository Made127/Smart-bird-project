#ifndef AI_H
#define AI_H

#include <stdint.h>

// 初始化服务器连接参数。
void ai_init(const char *server_ip, int server_port);
// 启动服务器回复音频轮询和定时传感器采集。
void ai_start_remote_audio(void);
// 主动采集一次传感器数据并上报。
int ai_collect_now(const char *source);
// 上传一段 16-bit PCM 单声道音频并播放服务器返回结果。
int ai_send_audio(int16_t *audio, int len);

#endif
