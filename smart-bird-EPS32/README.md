# bird_test voice AI

这个目录是独立测试版本，不会修改 `../BIRD` 和 `../Asr_server`。

## 环境变量

本项目不要在脚本里写死本机安装路径，统一从环境变量读取：

```powershell
$env:IDF_PATH="你的 ESP-IDF 目录"
$env:IDF_TOOLS_PATH="默认使用 BIRD_PROJECT_ROOT\.deps\espressif"
$env:IDF_PYTHON="你的 ESP-IDF Python.exe"
$env:IDF_PORT="你的 ESP32 串口号，例如 COM7"
$env:BIRD_PROJECT_ROOT="本项目根目录，也就是 smart bird/smart bird"
$env:BIRD_TEST_HOME="本 bird_test 工程目录"
$env:BIRD_DEPS_DIR="项目内依赖目录，默认是 BIRD_PROJECT_ROOT\.deps"
$env:BIRD_SERVER_VENV_DIR="服务端 Python 虚拟环境目录，默认是 BIRD_PROJECT_ROOT\.deps\python\server"
$env:BIRD_WIFI_SSID="你的WiFi"
$env:BIRD_WIFI_PASSWORD="你的WiFi密码"
$env:BIRD_SERVER_IP="你的电脑IP"
$env:BIRD_SERVER_PORT="8010"
```

`IDF_PYTHON` 是给 VS Code 和脚本定位 Python 用的；如果没有设置，脚本会继续尝试 `python` 或 `py`。
服务端 `.env` 中的相对路径默认以 `BIRD_PROJECT_ROOT` 为基准。项目根目录的 `use_project_deps.ps1` 会把 pip 缓存、FunASR/ModelScope/HuggingFace 模型缓存、npm 缓存、Gradle 缓存和 ESP-IDF tools 默认放到 `.deps` 目录。
如果要让 ESP-IDF 也跟项目一起迁移，把 ESP-IDF 本体放到 `.deps\esp-idf`，把 ESP-IDF tools 放到 `.deps\espressif`。
如果临时想继续使用系统里已有的依赖目录，可以在加载脚本前设置 `$env:BIRD_KEEP_SYSTEM_DEPS="1"`。

## 做了什么

- ESP32-S3 从麦克风 I2S 读取 16 kHz PCM。
- 简单 VAD 检测到说话结束后，把 PCM 上传到电脑服务端。
- 服务端执行 ASR -> DeepSeek -> TTS。
- 服务端返回 16 kHz 单声道 `pcm_s16le`。
- ESP32-S3 下载回答音频后直接通过 I2S 扬声器播放。

## 先配置电脑服务端

推荐直接运行项目根目录的一键配置脚本，它会创建 `.deps`、复制服务端 `.env`、创建项目内 Python 虚拟环境并安装依赖，同时安装 App 的 npm 依赖：

```powershell
cd $env:BIRD_PROJECT_ROOT
.\setup_project.ps1
```

国内网络慢时可以使用镜像：

```powershell
.\setup_project.ps1 -UseChinaMirror
```

只想生成环境变量和目录、不安装依赖时：

```powershell
.\setup_project.ps1 -NoInstall
```

```powershell
cd $env:BIRD_TEST_HOME
copy .\server\.env.example .\server\.env
notepad .\server\.env
.\run_server.ps1
```

如果默认 PyPI 证书或速度有问题，用国内镜像：

```powershell
$env:PIP_INDEX_URL="https://pypi.tuna.tsinghua.edu.cn/simple"
$env:PIP_TRUSTED_HOST="pypi.tuna.tsinghua.edu.cn"
.\run_server.ps1
```

如果需要手动进入服务端虚拟环境，默认路径在项目内：

```powershell
& "$env:BIRD_PROJECT_ROOT\.deps\python\server\Scripts\Activate.ps1"
```

至少填写：

- `DEEPSEEK_API_KEY`

TTS 默认使用 Windows 本机 SAPI 语音合成，不需要额外 Key。手机网页上传 `webm/mp4/ogg` 录音时需要 `ffmpeg` 解码，建议在 `.env` 里通过环境变量指定：

```text
FFMPEG_PATH=FormatFactory/ffmpeg.exe
```

本地中文 ASR 依赖已经写入 `server/requirements.txt`，也可以单独安装：

```powershell
& "$env:BIRD_PROJECT_ROOT\.deps\python\server\Scripts\python.exe" -m pip install funasr modelscope torch torchaudio
```

如果只是先测试链路，可以在 `.env` 里临时设置：

```text
ASR_PROVIDER=fallback
ASR_FALLBACK_TEXT=今天天气怎么样
```

启动：

```powershell
cd $env:BIRD_TEST_HOME
.\run_server.ps1
```

如果必须绕过启动脚本直接运行，也要先加载项目依赖环境：

```powershell
. "$env:BIRD_PROJECT_ROOT\use_project_deps.ps1"
cd "$env:BIRD_TEST_HOME\server"
& "$env:BIRD_SERVER_VENV_DIR\Scripts\python.exe" app.py
```

确认电脑 IP，例如 `192.168.92.131`，端口默认 `8010`。

## 再配置 ESP32 工程

打开：

```text
.\components\Config\include\bird_test_config.h
```

可以直接修改头文件，或者通过上面的同名环境变量在构建时覆盖：

```c
#define BIRD_WIFI_SSID      "你的WiFi"
#define BIRD_WIFI_PASSWORD  "你的WiFi密码"
#define BIRD_SERVER_IP      "你的电脑IP"
```

编译烧录：

```powershell
cd $env:BIRD_TEST_HOME
. "$env:BIRD_PROJECT_ROOT\use_project_deps.ps1"
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

如果普通终端提示找不到 `idf.py`，用这个脚本打开串口：

```powershell
.\run_monitor.ps1
```

也可以通过环境变量或参数指定串口：

```powershell
$env:IDF_PORT="COM8"
.\run_monitor.ps1 -Port COM8
```

## 引脚沿用原 BIRD

麦克风：

- SCK: GPIO10
- WS: GPIO11
- SD: GPIO12

扬声器 MAX98357A：

- BCLK: GPIO15
- LRC: GPIO16
- DOUT: GPIO17

## 国内服务选择

- LLM：默认 DeepSeek。
- ASR：优先 FunASR + ModelScope，本地推理，不把录音传到国外服务。
- TTS：默认 Windows 本机 SAPI；如果你有百度语音合成 Key，把 `.env` 改为 `TTS_PROVIDER=baidu` 并填写 `BAIDU_API_KEY`、`BAIDU_SECRET_KEY`。

## 当前真实 AI 模式

当前已配置为：

- ASR：FunASR 本地中文识别
- LLM：Qwen2.5-0.5B-Instruct 本地推理
- TTS：Windows SAPI

运行 `.\run_server.ps1` 后，需要先等待：

```text
[STARTUP] AI models ready
```

出现后再对麦克风说话。此时服务端输出的 `[ASR RESULT]` 是实际识别内容，`[AI RESULT]` 是 Qwen 根据问题生成的回答，不再是固定测试句。
