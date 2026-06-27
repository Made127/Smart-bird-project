$ErrorActionPreference = "Stop"

# 定位工程根目录和 Flask 服务目录。
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $root
$projectEnv = Join-Path $projectRoot "use_project_deps.ps1"
if (Test-Path -LiteralPath $projectEnv) {
    # 这行很重要：先加载项目内依赖目录，后续创建 venv 和下载模型才不会写到用户目录。
    . $projectEnv
}

$server = Join-Path $root "server"
$serverVenv = if ([string]::IsNullOrWhiteSpace($env:BIRD_SERVER_VENV_DIR)) {
    Join-Path $server ".venv"
} else {
    $env:BIRD_SERVER_VENV_DIR
}

# 优先使用项目内依赖目录中的虚拟环境，其次兼容旧的 server\.venv。
$pythonProjectScripts = Join-Path $serverVenv "Scripts\python.exe"
$pythonProjectBin = Join-Path $serverVenv "bin\python.exe"
$pythonAi = Join-Path $server ".venv_ai\Scripts\python.exe"
$pythonBin = Join-Path $server ".venv\bin\python.exe"
$pythonScripts = Join-Path $server ".venv\Scripts\python.exe"

# 检查指定 python.exe 是否真的可执行；旧虚拟环境里可能存在文件但指向失效路径。
function Test-PythonWorks {
    param(
        [string]$Path
    )

    if (!(Test-Path -LiteralPath $Path)) {
        return $false
    }

    try {
        # 只取退出码，不把版本信息打印到控制台，避免启动脚本输出太杂。
        & $Path --version *> $null
        return $LASTEXITCODE -eq 0
    } catch {
        return $false
    }
}

# 查找可用于创建虚拟环境的基础 Python。
function Find-BasePython {
    $candidates = @()

    # 最高优先级：用户显式配置的 PYTHON 环境变量。
    if (-not ([string]::IsNullOrWhiteSpace($env:PYTHON))) {
        $candidates += $env:PYTHON
    }

    # Windows 官方 Python Launcher，通常能找到真实安装的 Python。
    $pyCommand = Get-Command py -ErrorAction SilentlyContinue
    if ($pyCommand) {
        $candidates += $pyCommand.Source
    }

    # 从 PATH 查找 python，但跳过 WindowsApps 占位程序。
    $pythonCommands = Get-Command python -ErrorAction SilentlyContinue
    foreach ($command in @($pythonCommands)) {
        # 这行很重要：WindowsApps 的 python.exe 只是商店占位符，不能稳定创建 venv。
        if ($command -and $command.Source -notlike "*\WindowsApps\python.exe") {
            $candidates += $command.Source
        }
    }

    # 返回第一个真正能执行 --version 的 Python。
    foreach ($candidate in $candidates) {
        if (Test-PythonWorks $candidate) {
            return $candidate
        }
    }

    throw "Cannot find a working Python. Set PYTHON to python.exe or add Python to PATH."
}

# 确保 Flask 服务所需的基础 Python 包已经安装。
function Ensure-ServerRequirements {
    param(
        [string]$PythonPath
    )

    # 先用 import 做快速检测；已经安装时不重复 pip install。
    & $PythonPath -c "import flask, requests, dotenv" *> $null
    if ($LASTEXITCODE -eq 0) {
        return
    }

    # 缺包时按 requirements.txt 安装基础服务依赖。
    $requirements = Join-Path $server "requirements.txt"
    if (!(Test-Path -LiteralPath $requirements)) {
        throw "Server requirements.txt not found: $requirements"
    }

    Write-Host "installing server requirements..."
    & $PythonPath -m pip install -r $requirements
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to install server requirements."
    }
}

# 从 .env 读取简单的 KEY=VALUE 配置，用于判断是否需要本地语音识别依赖。
function Get-DotEnvValue {
    param(
        [string]$Name,
        [string]$DefaultValue
    )

    $envPath = Join-Path $server ".env"
    if (!(Test-Path -LiteralPath $envPath)) {
        return $DefaultValue
    }

    foreach ($line in Get-Content -LiteralPath $envPath) {
        $trimmed = $line.Trim()
        if ($trimmed.StartsWith("#") -or !$trimmed.Contains("=")) {
            continue
        }

        $parts = $trimmed.Split("=", 2)
        if ($parts[0].Trim() -eq $Name) {
            return $parts[1].Trim()
        }
    }

    return $DefaultValue
}

# 如果启用了 FunASR，必须确认大依赖也在当前 Python 环境里，否则服务会启动但识别结果一直为空。
function Ensure-VoiceRequirements {
    param(
        [string]$PythonPath
    )

    $asrProvider = if (-not ([string]::IsNullOrWhiteSpace($env:ASR_PROVIDER))) {
        $env:ASR_PROVIDER
    } else {
        Get-DotEnvValue "ASR_PROVIDER" "funasr"
    }

    if ($asrProvider.ToLowerInvariant() -ne "funasr") {
        return
    }

    & $PythonPath -c "import funasr, modelscope, torch, torchaudio" *> $null
    if ($LASTEXITCODE -eq 0) {
        return
    }

    # 这行很重要：ASR 缺依赖时不能静默继续，否则用户只会看到“没有听清楚”。
    Write-Host "installing FunASR voice requirements..."
    $requirements = Join-Path $server "requirements.txt"
    & $PythonPath -m pip install -r $requirements
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to install FunASR voice requirements."
    }
}

# 如果虚拟环境不存在，则自动创建一个基础 venv。
if (Test-PythonWorks $pythonProjectScripts) {
    # 项目内默认依赖目录，便于整目录迁移。
    $python = $pythonProjectScripts
} elseif (Test-PythonWorks $pythonProjectBin) {
    # 兼容类 Unix 风格目录结构。
    $python = $pythonProjectBin
} elseif (Test-PythonWorks $pythonAi) {
    # 兼容旧的 AI 可选依赖虚拟环境。
    $python = $pythonAi
} elseif (Test-PythonWorks $pythonBin) {
    # 兼容类 Unix 风格目录结构。
    $python = $pythonBin
} elseif (Test-PythonWorks $pythonScripts) {
    # Windows venv 默认把 python.exe 放在 Scripts 目录。
    $python = $pythonScripts
} else {
    Write-Host "server venv not found, creating it in: $serverVenv"
    $basePython = Find-BasePython
    # 用真实 Python 创建虚拟环境，不能直接依赖 PATH 里的 python 占位符。
    & $basePython -m venv $serverVenv
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to create server venv at $serverVenv with Python: $basePython"
    }

    # 创建完成后重新定位虚拟环境内的 python.exe。
    if (Test-PythonWorks $pythonProjectScripts) {
        $python = $pythonProjectScripts
    } elseif (Test-PythonWorks $pythonProjectBin) {
        $python = $pythonProjectBin
    } else {
        throw "Cannot find python.exe in server venv: $serverVenv"
    }

    # 新建虚拟环境通常没有 Flask 等依赖，这里立即安装。
    $requirements = Join-Path $server "requirements.txt"
    if (Test-Path -LiteralPath $requirements) {
        Write-Host "installing server requirements..."
        & $python -m pip install -r $requirements
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to install server requirements."
        }
    }
}

# 即使虚拟环境已存在，也要确认依赖完整，防止缺包导致服务启动失败。
Ensure-ServerRequirements $python
Ensure-VoiceRequirements $python

# 进入 server 目录运行 Flask 服务，结束后恢复原来的工作目录。
Push-Location $server
try {
    & $python "app.py"
} finally {
    Pop-Location
}





