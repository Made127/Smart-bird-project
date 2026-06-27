# 云端 AI 配置

当前语音链路：

1. ESP32 麦克风录音。
2. `bird_test/server` 在电脑上完成中文语音识别。
3. 识别文字发送到 DeepSeek 云端大模型。
4. 电脑合成回复语音并返回 ESP32 扬声器播放。

## 配置 API Key

1. 登录 DeepSeek 开放平台并创建 API Key。
2. 打开 `server/.env`。
3. 填写：

```text
LLM_PROVIDER=deepseek
DEEPSEEK_API_KEY=你的API_Key
DEEPSEEK_BASE_URL=https://api.deepseek.com
DEEPSEEK_MODEL=deepseek-v4-flash
```

不要把 API Key 发给别人，也不要把带有真实 Key 的 `.env` 上传到代码仓库。

## 启动

在 `bird_test` 根目录运行：

```powershell
.\run_server.ps1
```

看到下面内容表示已切换到云端大模型：

```text
[STARTUP] DeepSeek cloud ready
```

每次提问时看到下面内容表示请求已发送并收到云端回答：

```text
[LLM CLOUD] request
[LLM CLOUD] response received
```

ESP32 固件和服务器地址不需要因这次切换而重新修改。
