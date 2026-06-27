param(
    # ESP32 串口号；优先使用命令参数，其次使用 IDF_PORT 环境变量。
    [string]$Port = $env:IDF_PORT
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($Port)) {
    $Port = "COM7"
}

# 定位当前工程根目录，并优先使用本工程自检脚本配置 ESP-IDF 环境。
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$localEnv = Join-Path $root "check_esp32_env.ps1"
if (Test-Path -LiteralPath $localEnv) {
    # 这行很重要：monitor 只加载环境，不触发 ESP-IDF 下载或安装。
    . $localEnv -NoInstall -Port $Port
} else {
    $projectEnv = Join-Path (Split-Path -Parent $root) "use_project_deps.ps1"
    if (Test-Path -LiteralPath $projectEnv) {
        . $projectEnv
    }
}

if ([string]::IsNullOrWhiteSpace($env:IDF_PATH)) {
    # 这行很重要：没有 IDF_PATH 就无法定位 idf.py，后续 monitor 一定会失败。
    throw "IDF_PATH is not set. Run .\check_esp32_env.ps1 first, or install ESP-IDF under smart-bird-EPS32\.deps\esp-idf."
}

# 根据环境变量定位 ESP-IDF 自带的 idf.py，避免脚本绑定到某一台电脑的固定路径。
$idfPy = Join-Path $env:IDF_PATH "tools\idf.py"
if (!(Test-Path -LiteralPath $idfPy)) {
    throw "idf.py not found: $idfPy"
}

if ([string]::IsNullOrWhiteSpace($env:IDF_TARGET)) {
    # 这行很重要：本项目按 ESP32-S3 编译，未设置目标芯片时使用默认值。
    $env:IDF_TARGET = "esp32s3"
}

# 优先使用 IDF_PYTHON 环境变量；没有时再使用 PATH 中的 python。
$pythonCommand = $null
if (-not ([string]::IsNullOrWhiteSpace($env:IDF_PYTHON))) {
    if (!(Test-Path -LiteralPath $env:IDF_PYTHON)) {
        # 这行很重要：IDF_PYTHON 配错时要立即停止，否则后面会用错误解释器运行 idf.py。
        throw "IDF_PYTHON is set but the file does not exist: $env:IDF_PYTHON"
    }
    $pythonCommand = [PSCustomObject]@{ Source = $env:IDF_PYTHON }
}

if (!$pythonCommand) {
    $pythonCommand = Get-Command python -ErrorAction SilentlyContinue
}
if (!$pythonCommand) {
    $pythonCommand = Get-Command py -ErrorAction SilentlyContinue
}
if (!$pythonCommand) {
    # 这行很重要：idf.py 本质是 Python 脚本，没有 Python 就无法启动 monitor。
    throw "Python was not found in PATH. Please install Python or run the ESP-IDF environment export first."
}

# 在工程根目录执行 idf.py monitor，结束后恢复原目录。
Push-Location $root
try {
    # 这行很重要：通过当前环境中的 Python 运行 idf.py，兼容不同 ESP-IDF 安装位置。
    & $pythonCommand.Source $idfPy -p $Port monitor
} finally {
    Pop-Location
}





