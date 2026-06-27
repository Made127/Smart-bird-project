import os
import shutil
import subprocess
import threading
import time
import uuid
import wave
import hashlib
import json
import sqlite3
from collections import deque
from pathlib import Path

import requests
from dotenv import load_dotenv
from flask import Flask, jsonify, redirect, request, send_file, send_from_directory

load_dotenv()

APP_DIR = Path(__file__).resolve().parent


def resolve_path(value: str | Path, base_dir: Path) -> Path:
    expanded = Path(os.path.expandvars(os.path.expanduser(str(value))))
    if expanded.is_absolute():
        return expanded.resolve()
    return (base_dir / expanded).resolve()


def env_path(name: str, default: str | Path, *, fallback_base: Path | None = None) -> Path:
    raw_value = os.getenv(name, "").strip()
    value = raw_value if raw_value else default
    path = resolve_path(value, PROJECT_ROOT)

    if raw_value and fallback_base is not None and not path.exists():
        legacy_path = resolve_path(value, fallback_base)
        if legacy_path.exists():
            return legacy_path

    return path


# 服务端目录、手机网页目录、运行缓存目录和历史数据文件位置均可通过环境变量覆盖。
# 这行很重要：BIRD_PROJECT_ROOT 是所有相对路径的基准，换机器时只需要改这个环境变量。
PROJECT_ROOT = resolve_path(os.getenv("BIRD_PROJECT_ROOT", APP_DIR.parent.parent), APP_DIR)
DEPS_DIR = env_path("BIRD_DEPS_DIR", ".deps")


def normalize_dependency_path_env(name: str, default: str | Path) -> None:
    path = env_path(name, default)
    path.mkdir(parents=True, exist_ok=True)
    # 这行很重要：模型库会直接读这些环境变量，必须在懒加载 FunASR/LLM 前改成项目内绝对路径。
    os.environ[name] = str(path)


# 模型和推理框架缓存默认放到项目 .deps 下，避免换电脑后依赖散落在用户目录。
normalize_dependency_path_env("HF_HOME", DEPS_DIR / "huggingface")
normalize_dependency_path_env("HF_HUB_CACHE", DEPS_DIR / "huggingface" / "hub")
normalize_dependency_path_env("TRANSFORMERS_CACHE", DEPS_DIR / "huggingface" / "transformers")
normalize_dependency_path_env("MODELSCOPE_CACHE", DEPS_DIR / "modelscope")
normalize_dependency_path_env("TORCH_HOME", DEPS_DIR / "torch")
normalize_dependency_path_env("XDG_CACHE_HOME", DEPS_DIR / "cache")

MOBILE_APP_DIR = env_path("BIRD_MOBILE_APP_DIR", APP_DIR.parent / "App", fallback_base=APP_DIR)
WORK_DIR = env_path("BIRD_SERVER_WORK_DIR", APP_DIR / "work", fallback_base=APP_DIR)
# 这行很重要：工作目录必须跟随 app.py 所在目录创建，避免项目移动后继续写入旧 bird_test 路径。
WORK_DIR.mkdir(parents=True, exist_ok=True)
CACHE_DIR = env_path("BIRD_SERVER_CACHE_DIR", WORK_DIR / "cache", fallback_base=APP_DIR)
CACHE_DIR.mkdir(parents=True, exist_ok=True)
HISTORY_PATH = env_path("BIRD_HISTORY_PATH", WORK_DIR / "sensor_history.json", fallback_base=APP_DIR)
DB_PATH = env_path("BIRD_DB_PATH", WORK_DIR / "bird_history.db", fallback_base=APP_DIR)

SAMPLE_RATE = 16000
CHANNELS = 1
SAMPLE_WIDTH = 2
COLLECT_INTERVAL_SECONDS = int(os.getenv("BIRD_COLLECT_INTERVAL_SECONDS", "7200"))

# Flask 服务和全局运行状态；设备队列用于手机端与 ESP32 之间异步传递音频/命令。
app = Flask(__name__)
_funasr_model = None
_local_llm_model = None
_local_llm_tokenizer = None
_pipeline_lock = threading.Lock()
_device_lock = threading.Lock()
_device_audio_queue = deque()
_device_command_queue = deque()
_sensor_history = deque(maxlen=500)
_device_status = {
    "last_seen": 0.0,
    "ir_raw": None,
    "lux": None,
    "soil_temperature": None,
    "soil_humidity": None,
    "soil_ec": None,
    "soil_salt": None,
    "soil_nitrogen": None,
    "soil_phosphorus": None,
    "soil_potassium": None,
    "soil_ph": None,
    "soil_valid": False,
    "battery_voltage_mv": None,
    "battery_percent": None,
    "battery_low": False,
    "battery_charging": False,
    "battery_full": False,
    "last_collection_at": 0.0,
    "last_collection_source": None,
}


# 创建 SQLite 连接，并让查询结果可以按字段名访问。
def db_connect() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    return conn


# 初始化传感器历史数据库；旧库缺字段时会自动补列。
def init_database() -> None:
    with db_connect() as conn:
        conn.execute(
            """
            CREATE TABLE IF NOT EXISTS sensor_history (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                collected_at REAL NOT NULL,
                source TEXT NOT NULL,
                ir_raw INTEGER,
                lux REAL,
                soil_temperature REAL,
                soil_humidity REAL,
                soil_ec INTEGER,
                soil_salt INTEGER,
                soil_nitrogen INTEGER,
                soil_phosphorus INTEGER,
                soil_potassium INTEGER,
                soil_ph REAL,
                soil_valid INTEGER DEFAULT 0,
                battery_voltage_mv INTEGER,
                battery_percent INTEGER,
                battery_low INTEGER DEFAULT 0,
                battery_charging INTEGER DEFAULT 0,
                battery_full INTEGER DEFAULT 0,
                created_at REAL NOT NULL
            )
            """
        )
        existing_columns = {
            row["name"]
            for row in conn.execute("PRAGMA table_info(sensor_history)").fetchall()
        }
        extra_columns = {
            "soil_temperature": "REAL",
            "soil_humidity": "REAL",
            "soil_ec": "INTEGER",
            "soil_salt": "INTEGER",
            "soil_nitrogen": "INTEGER",
            "soil_phosphorus": "INTEGER",
            "soil_potassium": "INTEGER",
            "soil_ph": "REAL",
            "soil_valid": "INTEGER DEFAULT 0",
            "battery_voltage_mv": "INTEGER",
            "battery_percent": "INTEGER",
            "battery_low": "INTEGER DEFAULT 0",
            "battery_charging": "INTEGER DEFAULT 0",
            "battery_full": "INTEGER DEFAULT 0",
        }
        for name, column_type in extra_columns.items():
            if name not in existing_columns:
                conn.execute(f"ALTER TABLE sensor_history ADD COLUMN {name} {column_type}")
        conn.execute(
            "CREATE INDEX IF NOT EXISTS idx_sensor_history_collected_at "
            "ON sensor_history(collected_at)"
        )
        conn.commit()


# 写入一条传感器采样记录。
def db_insert_sensor_sample(sample: dict) -> None:
    with db_connect() as conn:
        conn.execute(
            """
            INSERT INTO sensor_history (
                collected_at, source, ir_raw, lux,
                soil_temperature, soil_humidity, soil_ec, soil_salt,
                soil_nitrogen, soil_phosphorus, soil_potassium, soil_ph,
                soil_valid, battery_voltage_mv, battery_percent,
                battery_low, battery_charging, battery_full, created_at
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            """,
            (
                float(sample["time"]),
                str(sample.get("source") or "unknown"),
                sample.get("ir_raw"),
                sample.get("lux"),
                sample.get("soil_temperature"),
                sample.get("soil_humidity"),
                sample.get("soil_ec"),
                sample.get("soil_salt"),
                sample.get("soil_nitrogen"),
                sample.get("soil_phosphorus"),
                sample.get("soil_potassium"),
                sample.get("soil_ph"),
                1 if sample.get("soil_valid") else 0,
                sample.get("battery_voltage_mv"),
                sample.get("battery_percent"),
                1 if sample.get("battery_low") else 0,
                1 if sample.get("battery_charging") else 0,
                1 if sample.get("battery_full") else 0,
                time.time(),
            ),
        )
        conn.commit()


# 按时间倒序读取最近的历史记录，再反转成时间正序返回给前端。
def db_read_sensor_history(limit: int = 100) -> list[dict]:
    with db_connect() as conn:
        rows = conn.execute(
            """
            SELECT
                collected_at, source, ir_raw, lux,
                soil_temperature, soil_humidity, soil_ec, soil_salt,
                soil_nitrogen, soil_phosphorus, soil_potassium, soil_ph,
                soil_valid, battery_voltage_mv, battery_percent,
                battery_low, battery_charging, battery_full
            FROM sensor_history
            ORDER BY collected_at DESC
            LIMIT ?
            """,
            (limit,),
        ).fetchall()
    return [
        {
            "time": float(row["collected_at"]),
            "source": row["source"],
            "ir_raw": row["ir_raw"],
            "lux": row["lux"],
            "soil_temperature": row["soil_temperature"],
            "soil_humidity": row["soil_humidity"],
            "soil_ec": row["soil_ec"],
            "soil_salt": row["soil_salt"],
            "soil_nitrogen": row["soil_nitrogen"],
            "soil_phosphorus": row["soil_phosphorus"],
            "soil_potassium": row["soil_potassium"],
            "soil_ph": row["soil_ph"],
            "soil_valid": bool(row["soil_valid"]),
            "battery_voltage_mv": row["battery_voltage_mv"],
            "battery_percent": row["battery_percent"],
            "battery_low": bool(row["battery_low"]),
            "battery_charging": bool(row["battery_charging"]),
            "battery_full": bool(row["battery_full"]),
        }
        for row in reversed(rows)
    ]


# 返回数据库里的历史记录总数。
def db_sensor_history_count() -> int:
    with db_connect() as conn:
        row = conn.execute("SELECT COUNT(*) AS count FROM sensor_history").fetchone()
    return int(row["count"] if row else 0)


# 启动时加载历史数据；优先读 SQLite，旧 JSON 文件会迁移进数据库。
def load_sensor_history() -> None:
    init_database()
    db_items = db_read_sensor_history(500)
    if db_items:
        _sensor_history.clear()
        _sensor_history.extend(db_items)
        last = db_items[-1]
        _device_status["last_collection_at"] = float(last.get("time") or 0.0)
        _device_status["last_collection_source"] = last.get("source")
        return

    if not HISTORY_PATH.exists():
        return
    try:
        data = json.loads(HISTORY_PATH.read_text(encoding="utf-8"))
        if isinstance(data, list):
            _sensor_history.clear()
            _sensor_history.extend(item for item in data[-500:] if isinstance(item, dict))
            for item in _sensor_history:
                db_insert_sensor_sample(item)
            if _sensor_history:
                last = _sensor_history[-1]
                _device_status["last_collection_at"] = float(last.get("time") or 0.0)
                _device_status["last_collection_source"] = last.get("source")
    except Exception as exc:
        print("[HISTORY] load failed:", repr(exc))


# 把内存中的最近历史样本保存为 JSON 备份。
def save_sensor_history() -> None:
    tmp_path = HISTORY_PATH.with_suffix(".tmp")
    tmp_path.write_text(
        json.dumps(list(_sensor_history), ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    tmp_path.replace(HISTORY_PATH)


load_sensor_history()


# 所有接口统一加 CORS 响应头，方便手机网页直接访问本服务。
@app.after_request
def add_cors_headers(response):
    # 这行很重要：手机网页和 ESP32 可能跨地址访问服务端接口，必须允许跨域。
    response.headers["Access-Control-Allow-Origin"] = "*"
    response.headers["Access-Control-Allow-Headers"] = "Content-Type"
    response.headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS"
    return response


# 把 ESP32 上传的裸 PCM 写成 WAV，供语音识别模型读取。
def write_wav_from_pcm(pcm_path: Path, wav_path: Path) -> None:
    pcm = pcm_path.read_bytes()
    with wave.open(str(wav_path), "wb") as wf:
        wf.setnchannels(CHANNELS)
        wf.setsampwidth(SAMPLE_WIDTH)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(pcm)


def find_ffmpeg() -> str:
    ffmpeg = os.getenv("FFMPEG_PATH", "").strip()
    if ffmpeg:
        ffmpeg_path = env_path("FFMPEG_PATH", ffmpeg, fallback_base=APP_DIR)
        return str(ffmpeg_path) if ffmpeg_path.exists() else ""

    bundled = (PROJECT_ROOT / "FormatFactory" / "ffmpeg.exe").resolve()
    if bundled.exists():
        return str(bundled)

    return shutil.which("ffmpeg") or ""


# 把手机 App 上传的音频格式转成 16k 单声道 WAV。
def decode_app_audio_to_wav(audio_path: Path, wav_path: Path) -> None:
    ffmpeg = find_ffmpeg()
    if not ffmpeg or not Path(ffmpeg).exists():
        raise RuntimeError("ffmpeg not found, set FFMPEG_PATH in .env")

    subprocess.run(
        [
            ffmpeg,
            "-y",
            "-i",
            str(audio_path),
            "-ac",
            str(CHANNELS),
            "-ar",
            str(SAMPLE_RATE),
            "-sample_fmt",
            "s16",
            str(wav_path),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )


# 语音识别入口；FunASR 失败时可按环境变量回退到固定文本。
def asr(audio_wav_path: Path) -> str:
    provider = os.getenv("ASR_PROVIDER", "funasr").lower()

    # 这行很重要：没有 ASR 模型时可切换 fallback，保证调试流程不被语音识别依赖阻塞。
    if provider == "fallback":
        return os.getenv("ASR_FALLBACK_TEXT", "你好")

    try:
        return asr_funasr(audio_wav_path)
    except Exception as exc:
        fallback = os.getenv("ASR_FALLBACK_TEXT", "").strip()
        print("[ASR] FunASR failed:", repr(exc))
        if fallback:
            print("[ASR] use fallback text:", fallback)
            return fallback
        return ""


# 使用 FunASR 本地模型识别音频。
def asr_funasr(audio_wav_path: Path) -> str:
    global _funasr_model

    if _funasr_model is None:
        from funasr import AutoModel

        _funasr_model = AutoModel(
            model="paraformer-zh",
            vad_model="fsmn-vad",
            punc_model="ct-punc",
            model_hub="ms",
            disable_update=True,
        )

    result = _funasr_model.generate(input=str(audio_wav_path), batch_size_s=60)
    if not result:
        return ""
    if isinstance(result, list):
        return "".join(item.get("text", "") for item in result).strip()
    return str(result)


# 大模型问答统一入口，根据环境变量选择本地 Qwen 或 DeepSeek 云端。
def ask_ai(text: str) -> str:
    provider = os.getenv("LLM_PROVIDER", "qwen_local").lower()
    if provider == "deepseek":
        return ask_deepseek(text)
    if provider == "qwen_local":
        return ask_local_qwen(text)
    if provider == "fallback":
        return ask_fallback(text)
    raise RuntimeError(f"unsupported LLM_PROVIDER: {provider}")


def ask_fallback(text: str) -> str:
    normalized = (text or "").replace(" ", "")
    hears_user = any(
        keyword in normalized
        for keyword in ("听得到", "听见", "听到", "能听", "能不能听")
    )
    if hears_user:
        return "我听得到你说话，语音识别已经正常。"

    # 这行很重要：没有本地模型或云端 Key 时仍返回可朗读文本，避免用户误以为麦克风坏了。
    return f"我听到你说：{text}"


# 对红外/光照类问题直接用最新传感器值回答，避免每次都调用大模型。
def sensor_reply(text: str, ir_raw: int | None, lux: float | None) -> str | None:
    normalized = text.lower().replace(" ", "")
    asks_ir = any(
        keyword in normalized
        for keyword in ("红外", "人体", "有人", "没人", "有没有人", "感应到人")
    )
    asks_light = any(
        keyword in normalized
        for keyword in (
            "光照",
            "亮度",
            "勒克斯",
            "lux",
            "亮不亮",
            "暗不暗",
            "明不明",
        )
    )
    asks_environment = any(
        keyword in normalized
        for keyword in ("传感器状态", "环境怎么样", "周围怎么样")
    )

    if not (asks_ir or asks_light or asks_environment):
        return None

    parts = []
    if asks_ir or asks_environment:
        if ir_raw is None:
            parts.append("红外传感器数据暂时不可用")
        elif ir_raw == 1:
            parts.append("红外传感器检测到有人")
        else:
            parts.append("红外传感器目前没有检测到人")

    if asks_light or asks_environment:
        if lux is None:
            parts.append("光照传感器数据暂时不可用")
        else:
            if lux < 10:
                description = "环境很暗"
            elif lux < 50:
                description = "环境比较暗"
            elif lux < 200:
                description = "环境光线偏暗"
            elif lux < 500:
                description = "环境光线正常"
            else:
                description = "环境比较明亮"
            parts.append(f"当前光照约为{lux:.1f}勒克斯，{description}")

    return "；".join(parts) + "。"


# 使用本地 Qwen 模型生成简短中文回复。
def ask_local_qwen(text: str) -> str:
    global _local_llm_model, _local_llm_tokenizer

    from transformers import AutoModelForCausalLM, AutoTokenizer

    model_path_value = os.getenv("LOCAL_QWEN_MODEL", "").strip()
    model_path = env_path("LOCAL_QWEN_MODEL", model_path_value) if model_path_value else None
    # 这行很重要：本地模型路径来自 .env，路径不存在时要明确报错。
    if not model_path or not model_path.exists():
        raise RuntimeError("LOCAL_QWEN_MODEL is missing or does not exist")

    if _local_llm_model is None:
        print("[LLM] loading local Qwen:", model_path)
        _local_llm_tokenizer = AutoTokenizer.from_pretrained(
            model_path,
            local_files_only=True,
        )
        _local_llm_model = AutoModelForCausalLM.from_pretrained(
            model_path,
            local_files_only=True,
            dtype="auto",
        )
        _local_llm_model.to("cpu")
        _local_llm_model.eval()
        print("[LLM] local Qwen ready")

    messages = [
        {
            "role": "system",
            "content": "你是一个中文语音助手。直接简短回答，不使用Markdown，最多两句话。",
        },
        {"role": "user", "content": text},
    ]
    prompt = _local_llm_tokenizer.apply_chat_template(
        messages,
        tokenize=False,
        add_generation_prompt=True,
    )
    inputs = _local_llm_tokenizer(prompt, return_tensors="pt")
    max_new_tokens = int(os.getenv("LOCAL_LLM_MAX_NEW_TOKENS", "80"))
    outputs = _local_llm_model.generate(
        **inputs,
        max_new_tokens=max_new_tokens,
        do_sample=False,
        repetition_penalty=1.05,
        pad_token_id=_local_llm_tokenizer.eos_token_id,
    )
    new_tokens = outputs[0][inputs["input_ids"].shape[1] :]
    reply = _local_llm_tokenizer.decode(new_tokens, skip_special_tokens=True).strip()
    return reply or "我暂时没有想好，请再问一次。"


# 调用 DeepSeek 云端接口生成回复。
def ask_deepseek(text: str) -> str:
    api_key = os.getenv("DEEPSEEK_API_KEY", "").strip()
    if not api_key or api_key == "replace_with_your_deepseek_key":
        raise RuntimeError("DEEPSEEK_API_KEY is not configured")

    base_url = os.getenv("DEEPSEEK_BASE_URL", "https://api.deepseek.com").rstrip("/")
    model = os.getenv("DEEPSEEK_MODEL", "deepseek-v4-flash")
    url = f"{base_url}/chat/completions"

    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }
    data = {
        "model": model,
        "messages": [
            {
                "role": "system",
                "content": "你是一个中文语音助手，回答要简短、自然、适合直接朗读。",
            },
            {"role": "user", "content": text},
        ],
        "stream": False,
        "thinking": {"type": "disabled"},
        "max_tokens": int(os.getenv("DEEPSEEK_MAX_TOKENS", "100")),
    }

    try:
        print(f"[LLM CLOUD] request model={model}")
        resp = requests.post(url, json=data, headers=headers, timeout=30)
        resp.raise_for_status()
        reply = resp.json()["choices"][0]["message"]["content"].strip()
        print("[LLM CLOUD] response received")
        return reply
    except Exception as exc:
        print("[LLM] DeepSeek failed:", repr(exc))
        raise RuntimeError("DeepSeek cloud request failed") from exc


# PowerShell 字符串单引号转义，供 SAPI TTS 脚本安全传参。
def ps_quote(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


# 使用 Windows SAPI 把文本合成为 wav 文件。
def sapi_tts_to_file(text: str, out_path: Path, job_id: str) -> None:
    text_path = WORK_DIR / f"{job_id}_reply.txt"
    text_path.write_text(text, encoding="utf-8")

    script = "; ".join(
        [
            "$ErrorActionPreference = 'Stop'",
            "Add-Type -AssemblyName System.Speech",
            "$s = New-Object System.Speech.Synthesis.SpeechSynthesizer",
            "$fmt = New-Object -TypeName System.Speech.AudioFormat.SpeechAudioFormatInfo "
            "-ArgumentList 16000, "
            "([System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen), "
            "([System.Speech.AudioFormat.AudioChannel]::Mono)",
            "$txt = Get-Content -Raw -Encoding UTF8 " + ps_quote(str(text_path)),
            # 这行很重要：直接让 SAPI 输出 ESP32 需要的 16k/16bit/单声道 WAV，避免依赖 ffmpeg 转码。
            "$s.SetOutputToWaveFile(" + ps_quote(str(out_path)) + ", $fmt)",
            "$s.Speak($txt)",
            "$s.Dispose()",
        ]
    )

    subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )


# 使用百度语音合成接口把文本合成为音频文件。
def baidu_tts_to_file(text: str, out_path: Path) -> None:
    api_key = os.getenv("BAIDU_API_KEY", "").strip()
    secret_key = os.getenv("BAIDU_SECRET_KEY", "").strip()
    cuid = os.getenv("BAIDU_CUID", "bird_test")
    if not api_key or not secret_key:
        raise RuntimeError("BAIDU_API_KEY/BAIDU_SECRET_KEY is empty")

    token_resp = requests.post(
        "https://aip.baidubce.com/oauth/2.0/token",
        params={
            "grant_type": "client_credentials",
            "client_id": api_key,
            "client_secret": secret_key,
        },
        timeout=15,
    )
    token_resp.raise_for_status()
    token = token_resp.json()["access_token"]

    audio_resp = requests.post(
        "https://tsn.baidu.com/text2audio",
        data={
            "tex": text,
            "tok": token,
            "cuid": cuid,
            "ctp": 1,
            "lan": "zh",
            "spd": 5,
            "pit": 5,
            "vol": 7,
            "per": 0,
            "aue": 3,
        },
        timeout=30,
    )
    audio_resp.raise_for_status()
    content_type = audio_resp.headers.get("Content-Type", "")
    if "audio" not in content_type:
        raise RuntimeError(audio_resp.text[:200])
    out_path.write_bytes(audio_resp.content)


def try_decode_wav_to_pcm(audio_path: Path, pcm_path: Path) -> bool:
    try:
        with wave.open(str(audio_path), "rb") as wf:
            channels = wf.getnchannels()
            sample_width = wf.getsampwidth()
            sample_rate = wf.getframerate()
            if (
                channels != CHANNELS
                or sample_width != SAMPLE_WIDTH
                or sample_rate != SAMPLE_RATE
            ):
                return False
            pcm_path.write_bytes(wf.readframes(wf.getnframes()))
            return True
    except (FileNotFoundError, wave.Error):
        return False


# 把 TTS 生成的 wav/mp3 等格式转成 ESP32 可直接播放的裸 PCM。
def decode_audio_to_pcm(audio_path: Path, pcm_path: Path) -> None:
    # SAPI 已经按 16k/16bit/单声道输出 WAV 时，直接剥离 WAV 头即可，不需要 ffmpeg。
    if try_decode_wav_to_pcm(audio_path, pcm_path):
        return

    ffmpeg = find_ffmpeg()
    if not ffmpeg or not Path(ffmpeg).exists():
        raise RuntimeError("ffmpeg not found, set FFMPEG_PATH in .env")

    subprocess.run(
        [
            ffmpeg,
            "-y",
            "-i",
            str(audio_path),
            "-ac",
            str(CHANNELS),
            "-ar",
            str(SAMPLE_RATE),
            "-f",
            "s16le",
            str(pcm_path),
        ],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )


# 文本转 PCM，按文本和 TTS 提供商做缓存，减少重复合成耗时。
def tts_to_pcm(text: str, job_id: str) -> Path:
    cache_key = hashlib.sha1(
        f"{os.getenv('TTS_PROVIDER', 'sapi').lower()}:{text}".encode("utf-8")
    ).hexdigest()
    cached_pcm = CACHE_DIR / f"{cache_key}.pcm"
    if cached_pcm.exists() and cached_pcm.stat().st_size > 0:
        print("[TTS] use cache", cached_pcm.name)
        return cached_pcm

    provider = os.getenv("TTS_PROVIDER", "sapi").lower()
    encoded_path = WORK_DIR / f"{job_id}_tts.bin"
    pcm_path = WORK_DIR / f"{job_id}_reply.pcm"

    try:
        if provider == "baidu":
            baidu_tts_to_file(text, encoded_path)
        elif provider == "edge":
            raise RuntimeError("edge TTS is not installed in this lightweight server")
        else:
            sapi_tts_to_file(text, encoded_path, job_id)
        decode_audio_to_pcm(encoded_path, pcm_path)
        shutil.copyfile(pcm_path, cached_pcm)
        return pcm_path
    except Exception as exc:
        print("[TTS] failed:", repr(exc))
        raise


# 服务启动时预加载 ASR/LLM，避免第一次对话等待过久。
def preload_models() -> None:
    if os.getenv("PRELOAD_MODELS", "true").lower() not in {"1", "true", "yes"}:
        return

    print("[STARTUP] preparing voice AI, please wait...")
    if os.getenv("ASR_PROVIDER", "funasr").lower() == "funasr":
        try:
            from funasr import AutoModel

            global _funasr_model
            _funasr_model = AutoModel(
                model="paraformer-zh",
                vad_model="fsmn-vad",
                punc_model="ct-punc",
                model_hub="ms",
                disable_update=True,
            )
        except ModuleNotFoundError as exc:
            # 这行很重要：FunASR 是可选大依赖，缺失时不能让整个 Flask 服务启动失败。
            print("[STARTUP] FunASR is not installed, ASR will use runtime fallback:", repr(exc))
        except Exception as exc:
            # 模型文件缺失或加载失败时也允许服务先启动，便于手机端和设备状态接口继续使用。
            print("[STARTUP] FunASR preload failed, ASR will retry at runtime:", repr(exc))

    llm_provider = os.getenv("LLM_PROVIDER", "qwen_local").lower()
    if llm_provider == "qwen_local":
        try:
            ask_local_qwen("你好")
        except Exception as exc:
            # 本地大模型路径或 transformers 依赖不完整时，启动服务不应被阻塞。
            print("[STARTUP] local Qwen preload failed, LLM will retry at runtime:", repr(exc))
    elif llm_provider == "deepseek":
        api_key = os.getenv("DEEPSEEK_API_KEY", "").strip()
        model = os.getenv("DEEPSEEK_MODEL", "deepseek-v4-flash")
        if not api_key or api_key == "replace_with_your_deepseek_key":
            print("[STARTUP] DeepSeek cloud selected, API key is not configured")
        else:
            print(f"[STARTUP] DeepSeek cloud ready, model={model}")
    elif llm_provider == "fallback":
        print("[STARTUP] fallback LLM selected, replies will echo recognized speech")
    else:
        raise RuntimeError(f"unsupported LLM_PROVIDER: {llm_provider}")

    print("[STARTUP] voice AI ready")


# 对土壤相关问题直接用最新探头数据回答。
def soil_reply(text: str, soil: dict | None) -> str | None:
    normalized = (text or "").lower().replace(" ", "")
    asks_soil = any(
        keyword in normalized
        for keyword in ("土壤", "探针", "湿度", "温度", "电导率", "盐分", "氮", "磷", "钾", "ph")
    )
    if not asks_soil:
        return None

    if not soil or not soil.get("soil_valid"):
        return "土壤探针暂时没有有效数据，请检查供电、磁吸头和串口接线。"

    parts = []
    if soil.get("soil_temperature") is not None:
        parts.append(f"土壤温度{soil['soil_temperature']:.1f}摄氏度")
    if soil.get("soil_humidity") is not None:
        parts.append(f"土壤湿度{soil['soil_humidity']:.1f}%")
    if soil.get("soil_ph") is not None:
        parts.append(f"PH {soil['soil_ph']:.1f}")
    if soil.get("soil_ec") is not None:
        parts.append(f"电导率{soil['soil_ec']}微西门子每厘米")
    if soil.get("soil_salt") is not None:
        parts.append(f"盐分{soil['soil_salt']}ppm")

    nutrients = []
    if soil.get("soil_nitrogen") is not None:
        nutrients.append(f"氮{soil['soil_nitrogen']}mg/kg")
    if soil.get("soil_phosphorus") is not None:
        nutrients.append(f"磷{soil['soil_phosphorus']}mg/kg")
    if soil.get("soil_potassium") is not None:
        nutrients.append(f"钾{soil['soil_potassium']}mg/kg")
    if nutrients:
        parts.append("养分" + "，".join(nutrients))

    return "，".join(parts) + "。"


# 对电量相关问题直接用最新电池数据回答。
def battery_reply(text: str, battery: dict | None) -> str | None:
    normalized = (text or "").lower().replace(" ", "")
    asks_battery = any(
        keyword in normalized
        for keyword in (
            "电量",
            "电池",
            "还有多少电",
            "多少电",
            "低电",
            "充电",
            "battery",
        )
    )
    if not asks_battery:
        return None

    if not battery or not battery.get("battery_valid"):
        return "暂时没有读到电量数据，请检查电池检测线。"

    percent = battery.get("battery_percent")
    voltage_mv = battery.get("battery_voltage_mv")
    low = bool(battery.get("battery_low")) or (
        isinstance(percent, (int, float)) and percent <= 5
    )

    if low:
        if isinstance(percent, (int, float)) and isinstance(voltage_mv, (int, float)):
            return f"电量低 请充电。当前电量约{int(percent)}%，电压{voltage_mv / 1000:.2f}伏。"
        return "电量低 请充电"

    parts = []
    if isinstance(percent, (int, float)):
        parts.append(f"当前电量约{int(percent)}%")
    if isinstance(voltage_mv, (int, float)):
        parts.append(f"电池电压{voltage_mv / 1000:.2f}伏")
    if battery.get("battery_charging"):
        parts.append("正在充电")
    elif battery.get("battery_full"):
        parts.append("电量已满")

    return "，".join(parts) + "。"


# 按优先级生成最终回复：电池、土壤、普通传感器，最后才调用大模型。
def create_reply(
    text: str,
    ir_raw: int | None,
    lux: float | None,
    soil: dict | None = None,
    battery: dict | None = None,
) -> str:
    if not text:
        return "我没有听清楚，请再说一遍。"

    reply = battery_reply(text, battery)
    if reply is not None:
        print("[BATTERY REPLY]", reply)
        return reply

    reply = soil_reply(text, soil)
    if reply is not None:
        print("[SOIL REPLY]", reply)
        return reply

    reply = sensor_reply(text, ir_raw, lux)
    if reply is not None:
        print("[SENSOR REPLY]", reply)
        return reply

    try:
        return ask_ai(text)
    except Exception as exc:
        print("[LLM] failed:", repr(exc))
        return "我连接人工智能服务失败了，请检查电脑服务。"


# 原子读取最新传感器和电池状态，供语音处理流程使用。
def latest_sensor_values() -> tuple[int | None, float | None, dict, dict]:
    with _device_lock:
        soil = {
            key: _device_status.get(key)
            for key in (
                "soil_temperature",
                "soil_humidity",
                "soil_ec",
                "soil_salt",
                "soil_nitrogen",
                "soil_phosphorus",
                "soil_potassium",
                "soil_ph",
                "soil_valid",
            )
        }
        battery = {
            key: _device_status.get(key)
            for key in (
                "battery_voltage_mv",
                "battery_percent",
                "battery_low",
                "battery_charging",
                "battery_full",
            )
        }
        battery["battery_valid"] = (
            battery.get("battery_voltage_mv") is not None
            or battery.get("battery_percent") is not None
        )
        return _device_status["ir_raw"], _device_status["lux"], soil, battery


# 从 ESP32 请求头解析红外和光照数据。
def parse_sensor_headers() -> tuple[int | None, float | None]:
    try:
        ir_raw = int(request.headers.get("X-IR-Raw", ""))
    except ValueError:
        ir_raw = None
    try:
        lux_x10 = int(request.headers.get("X-Light-Lux-X10", ""))
        lux = lux_x10 / 10.0 if lux_x10 >= 0 else None
    except ValueError:
        lux = None
    return ir_raw, lux


# 从 ESP32 请求头解析土壤探头数据。
def parse_soil_headers() -> dict:
    def int_header(name: str) -> int | None:
        try:
            return int(request.headers.get(name, ""))
        except ValueError:
            return None

    temp_x10 = int_header("X-Soil-Temp-X10")
    humidity_x10 = int_header("X-Soil-Humidity-X10")
    ph_x10 = int_header("X-Soil-Ph-X10")
    soil = {
        "soil_temperature": temp_x10 / 10.0 if temp_x10 is not None else None,
        "soil_humidity": humidity_x10 / 10.0 if humidity_x10 is not None else None,
        "soil_ec": int_header("X-Soil-Ec"),
        "soil_salt": int_header("X-Soil-Salt"),
        "soil_nitrogen": int_header("X-Soil-N"),
        "soil_phosphorus": int_header("X-Soil-P"),
        "soil_potassium": int_header("X-Soil-K"),
        "soil_ph": ph_x10 / 10.0 if ph_x10 is not None else None,
    }
    soil["soil_valid"] = any(value is not None for value in soil.values())
    return soil


# 从 ESP32 请求头解析电池数据。
def parse_battery_headers() -> dict:
    def int_header(name: str) -> int | None:
        try:
            return int(request.headers.get(name, ""))
        except ValueError:
            return None

    def bool_header(name: str) -> bool | None:
        value = int_header(name)
        if value is None:
            return None
        return value != 0

    voltage_mv = int_header("X-Battery-Voltage-Mv")
    percent = int_header("X-Battery-Percent")
    return {
        "battery_voltage_mv": voltage_mv,
        "battery_percent": percent,
        "battery_low": bool_header("X-Battery-Low"),
        "battery_charging": bool_header("X-Battery-Charging"),
        "battery_full": bool_header("X-Battery-Full"),
        "battery_valid": voltage_mv is not None or percent is not None,
    }


# 更新设备在线心跳和最近一次上传的传感器状态。
def update_device_heartbeat(
    ir_raw: int | None,
    lux: float | None,
    soil: dict | None = None,
    battery: dict | None = None,
) -> None:
    _device_status["last_seen"] = time.time()
    _device_status["ir_raw"] = ir_raw
    _device_status["lux"] = lux
    if soil and soil.get("soil_valid"):
        for key, value in soil.items():
            _device_status[key] = value
    if battery and battery.get("battery_valid"):
        for key, value in battery.items():
            if key != "battery_valid" and value is not None:
                _device_status[key] = value


# 保存一次传感器采集结果到内存、数据库和 JSON 备份。
def record_sensor_sample(
    source: str,
    ir_raw: int | None,
    lux: float | None,
    soil: dict | None = None,
    battery: dict | None = None,
) -> dict:
    now = time.time()
    sample = {
        "time": now,
        "source": source or "unknown",
        "ir_raw": ir_raw,
        "lux": lux,
        **(soil or {}),
        **(battery or {}),
    }
    with _device_lock:
        update_device_heartbeat(ir_raw, lux, soil, battery)
        _device_status["last_collection_at"] = now
        _device_status["last_collection_source"] = sample["source"]
        _sensor_history.append(sample)
        db_insert_sensor_sample(sample)
        try:
            save_sensor_history()
        except Exception as exc:
            print("[HISTORY] save failed:", repr(exc))
    print("[COLLECT]", sample)
    return sample


# 把手机端命令放入队列，等待 ESP32 轮询取走。
def queue_device_command(command: str, source: str = "app") -> dict:
    item = {
        "command": command,
        "source": source,
        "created_at": time.time(),
    }
    with _device_lock:
        _device_command_queue.append(item)
    print("[COMMAND] queued", item)
    return item


# 健康检查接口，返回服务配置和基础状态。
@app.route("/health", methods=["GET"])
def health():
    llm_provider = os.getenv("LLM_PROVIDER", "qwen_local").lower()
    cloud_configured = bool(os.getenv("DEEPSEEK_API_KEY", "").strip())
    return jsonify(
        {
            "ok": True,
            "sample_rate": SAMPLE_RATE,
            "collect_interval_seconds": COLLECT_INTERVAL_SECONDS,
            "llm_provider": llm_provider,
            "cloud_configured": cloud_configured
            if llm_provider == "deepseek"
            else None,
        }
    )


# 根路径给浏览器直接访问使用；没有这个路由时访问 http://地址:端口/ 会返回 404。
@app.route("/", methods=["GET"])
def root_index():
    # 这行很重要：手机网页实际入口是 /app/，根路径只负责把用户引过去。
    return redirect("/app/", code=302)


# 浏览器会自动请求 favicon.ico；项目当前没有该文件，返回 204 避免控制台反复刷 404。
@app.route("/favicon.ico", methods=["GET"])
def favicon():
    return "", 204


# 手机网页入口。
@app.route("/app/", methods=["GET"])
def mobile_app_index():
    return send_from_directory(MOBILE_APP_DIR, "index.html")


# 兼容不带尾斜杠的手机网页入口。
@app.route("/app", methods=["GET"])
def mobile_app_redirect():
    return redirect("/app/", code=302)


# 手机网页静态资源。
@app.route("/app/<path:name>", methods=["GET"])
def mobile_app_asset(name: str):
    return send_from_directory(MOBILE_APP_DIR, name)


# 手机端查询设备在线状态、传感器、电池和历史统计。
@app.route("/device/status", methods=["GET"])
def device_status():
    with _device_lock:
        last_seen = float(_device_status["last_seen"])
        last_collection_at = float(_device_status["last_collection_at"])
        next_collection_at = (
            last_collection_at + COLLECT_INTERVAL_SECONDS
            if last_collection_at > 0
            else 0.0
        )
        status = {
            "online": time.time() - last_seen < 5.0,
            "last_seen": last_seen,
            "ir_raw": _device_status["ir_raw"],
            "lux": _device_status["lux"],
            "soil_temperature": _device_status["soil_temperature"],
            "soil_humidity": _device_status["soil_humidity"],
            "soil_ec": _device_status["soil_ec"],
            "soil_salt": _device_status["soil_salt"],
            "soil_nitrogen": _device_status["soil_nitrogen"],
            "soil_phosphorus": _device_status["soil_phosphorus"],
            "soil_potassium": _device_status["soil_potassium"],
            "soil_ph": _device_status["soil_ph"],
            "soil_valid": _device_status["soil_valid"],
            "battery_voltage_mv": _device_status["battery_voltage_mv"],
            "battery_percent": _device_status["battery_percent"],
            "battery_low": _device_status["battery_low"],
            "battery_charging": _device_status["battery_charging"],
            "battery_full": _device_status["battery_full"],
            "pending_audio": len(_device_audio_queue),
            "pending_commands": len(_device_command_queue),
            "collect_interval_seconds": COLLECT_INTERVAL_SECONDS,
            "last_collection_at": last_collection_at,
            "last_collection_source": _device_status["last_collection_source"],
            "next_collection_at": next_collection_at,
            "history_count": db_sensor_history_count(),
        }
    return jsonify(status)


# ESP32 轮询待播放音频；没有音频时返回 204。
@app.route("/device/pending", methods=["GET"])
def device_pending():
    ir_raw, lux = parse_sensor_headers()
    soil = parse_soil_headers()
    battery = parse_battery_headers()

    with _device_lock:
        update_device_heartbeat(ir_raw, lux, soil, battery)
        reply_pcm = _device_audio_queue.popleft() if _device_audio_queue else None

    if reply_pcm is None:
        return "", 204

    print("[APP -> DEVICE] send", reply_pcm.name)
    response = send_file(
        reply_pcm,
        mimetype="application/octet-stream",
        conditional=False,
    )
    response.headers["X-Audio-Format"] = "pcm_s16le"
    response.headers["X-Sample-Rate"] = str(SAMPLE_RATE)
    response.headers["X-Channels"] = str(CHANNELS)
    return response


# ESP32 轮询手机端下发的命令。
@app.route("/device/command", methods=["GET"])
def device_command():
    ir_raw, lux = parse_sensor_headers()
    soil = parse_soil_headers()
    battery = parse_battery_headers()
    with _device_lock:
        update_device_heartbeat(ir_raw, lux, soil, battery)
        item = _device_command_queue.popleft() if _device_command_queue else None

    if item is None:
        return "", 204

    body = f"{item['command']}:{item.get('source') or 'app'}"
    print("[COMMAND] device fetch", body)
    return body, 200, {"Content-Type": "text/plain; charset=utf-8"}


# ESP32 主动上传一次传感器采集结果。
@app.route("/device/collect", methods=["POST", "OPTIONS"])
def device_collect():
    if request.method == "OPTIONS":
        return "", 204

    ir_raw, lux = parse_sensor_headers()
    soil = parse_soil_headers()
    battery = parse_battery_headers()
    source = request.headers.get("X-Collect-Source", "device")
    sample = record_sensor_sample(source, ir_raw, lux, soil, battery)
    return jsonify({"ok": True, "sample": sample})


# 手机端触发 ESP32 采集一次传感器数据。
@app.route("/app/collect", methods=["POST", "OPTIONS"])
def app_collect():
    if request.method == "OPTIONS":
        return "", 204

    item = queue_device_command("collect", "app")
    with _device_lock:
        last_seen = float(_device_status["last_seen"])
    return jsonify(
        {
            "ok": True,
            "queued": True,
            "command": item,
            "device_online": time.time() - last_seen < 5.0,
        }
    )


# 手机端读取传感器历史数据。
@app.route("/device/history", methods=["GET"])
def device_history():
    try:
        limit = int(request.args.get("limit", "100"))
    except ValueError:
        limit = 100
    limit = max(1, min(limit, 500))
    with _device_lock:
        items = db_read_sensor_history(limit)
    return jsonify({"ok": True, "history": items})


# 手机端上传语音，服务端生成回复后放入 ESP32 待播放队列。
@app.route("/app/voice", methods=["POST", "OPTIONS"])
def app_voice():
    if request.method == "OPTIONS":
        return "", 204

    audio = request.get_data()
    if not audio:
        return jsonify({"error": "empty audio"}), 400

    job_id = uuid.uuid4().hex
    source_path = WORK_DIR / f"{job_id}_phone_audio.bin"
    wav_path = WORK_DIR / f"{job_id}_phone_audio.wav"
    source_path.write_bytes(audio)

    try:
        with _pipeline_lock:
            decode_app_audio_to_wav(source_path, wav_path)
            text = asr(wav_path)
            ir_raw, lux, soil, battery = latest_sensor_values()
            reply = create_reply(text, ir_raw, lux, soil, battery)
            reply_pcm = tts_to_pcm(reply, job_id)
    except Exception as exc:
        print("[APP VOICE] failed:", repr(exc))
        return jsonify({"error": str(exc)}), 500

    with _device_lock:
        _device_audio_queue.clear()
        _device_audio_queue.append(reply_pcm)

    print("[APP ASR RESULT]", text)
    print("[APP AI RESULT]", reply)
    return jsonify(
        {
            "ok": True,
            "text": text,
            "reply": reply,
            "queued": True,
        }
    )


# ESP32 上传麦克风 PCM，服务端返回可直接播放的 PCM 回复。
@app.route("/upload", methods=["POST"])
def upload():
    audio = request.get_data()
    if not audio:
        return jsonify({"error": "empty audio"}), 400

    job_id = uuid.uuid4().hex
    pcm_path = WORK_DIR / f"{job_id}_mic.pcm"
    wav_path = WORK_DIR / f"{job_id}_mic.wav"
    pcm_path.write_bytes(audio)
    write_wav_from_pcm(pcm_path, wav_path)

    ir_raw, lux = parse_sensor_headers()
    soil = parse_soil_headers()
    battery = parse_battery_headers()
    with _device_lock:
        update_device_heartbeat(ir_raw, lux, soil, battery)
    print("[SENSOR DATA]", {"ir_raw": ir_raw, "lux": lux, **soil, **battery})

    with _pipeline_lock:
        text = asr(wav_path)
        print("[ASR RESULT]", text)
        reply = create_reply(text, ir_raw, lux, soil, battery)
        print("[AI RESULT]", reply)
        reply_pcm = tts_to_pcm(reply, job_id)

    response = send_file(
        reply_pcm,
        mimetype="application/octet-stream",
        conditional=False,
    )
    response.headers["X-Audio-Format"] = "pcm_s16le"
    response.headers["X-Sample-Rate"] = str(SAMPLE_RATE)
    response.headers["X-Channels"] = str(CHANNELS)
    return response


# 调试/回放接口：按文件名读取 work 或 cache 中的音频文件。
@app.route("/audio/<name>", methods=["GET"])
def audio(name: str):
    # 这行很重要：禁止路径分隔符，避免通过文件名参数读取 work/cache 之外的文件。
    if "/" in name or "\\" in name:
        return "bad name", 400
    path = WORK_DIR / name
    if not path.exists():
        path = CACHE_DIR / name
    if not path.exists():
        return "no audio", 404
    return send_file(path, mimetype="application/octet-stream")


# 直接运行本文件时启动 Flask 服务。
if __name__ == "__main__":
    host = os.getenv("BIRD_SERVER_HOST", "0.0.0.0")
    port = int(os.getenv("BIRD_SERVER_PORT", "8000"))
    preload_models()
    app.run(host=host, port=port)
