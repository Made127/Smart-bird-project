param(
    [switch]$UseChinaMirror,
    [switch]$NoInstall,
    [switch]$SkipPython,
    [switch]$SkipNode,
    [switch]$SkipAndroidCheck
)

$ErrorActionPreference = "Stop"

# 服务端根目录由脚本位置确定，整目录迁移后不需要修改绝对路径。
$ServerRoot = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ServerRoot)) {
    $ServerRoot = Split-Path -Parent $PSCommandPath
}
$ProjectRoot = Split-Path -Parent $ServerRoot
$ServerDir = Join-Path $ServerRoot "server"
$AppDir = Join-Path $ServerRoot "App"
$DepsDir = Join-Path $ServerRoot ".deps"
$ServerVenvDir = Join-Path $DepsDir "python\server"

function Write-Step {
    param([string]$Message)
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Ensure-Directory {
    param([string]$Path)
    if (!(Test-Path -LiteralPath $Path)) {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
    }
}

function Set-ProcessEnvPath {
    param(
        [string]$Name,
        [string]$Path
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    [Environment]::SetEnvironmentVariable($Name, $fullPath, "Process")
    Set-Item -Path ("Env:\" + $Name) -Value $fullPath
}

function Set-DotEnvValue {
    param(
        [string]$Path,
        [string]$Name,
        [string]$Value
    )

    $lines = @()
    if (Test-Path -LiteralPath $Path) {
        $lines = @(Get-Content -LiteralPath $Path)
    }

    $updated = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match ("^\s*" + [regex]::Escape($Name) + "\s*=")) {
            $lines[$i] = "$Name=$Value"
            $updated = $true
        }
    }

    if (!$updated) {
        $lines += "$Name=$Value"
    }

    # 这行很重要：.env 中的路径会覆盖代码默认值，必须同步到新目录结构。
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllLines($Path, $lines, $utf8NoBom)
}

function Test-PythonWorks {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path) -or !(Test-Path -LiteralPath $Path)) {
        return $false
    }

    try {
        & $Path --version *> $null
        return $LASTEXITCODE -eq 0
    } catch {
        return $false
    }
}

function Find-BasePython {
    $candidates = @()

    if (-not ([string]::IsNullOrWhiteSpace($env:PYTHON))) {
        $candidates += $env:PYTHON
    }

    $pyCommand = Get-Command py -ErrorAction SilentlyContinue
    if ($pyCommand) {
        $candidates += $pyCommand.Source
    }

    foreach ($command in @(Get-Command python -ErrorAction SilentlyContinue)) {
        # 这行很重要：WindowsApps 的 python.exe 通常只是商店占位符，不能创建稳定虚拟环境。
        if ($command -and $command.Source -notlike "*\WindowsApps\python.exe") {
            $candidates += $command.Source
        }
    }

    foreach ($candidate in $candidates) {
        if (Test-PythonWorks $candidate) {
            return $candidate
        }
    }

    return $null
}

function Get-ServerPython {
    $candidates = @(
        (Join-Path $ServerVenvDir "Scripts\python.exe"),
        (Join-Path $ServerVenvDir "bin\python.exe"),
        (Join-Path $ServerDir ".venv_ai\Scripts\python.exe"),
        (Join-Path $ServerDir ".venv\Scripts\python.exe")
    )

    foreach ($candidate in $candidates) {
        if (Test-PythonWorks $candidate) {
            return $candidate
        }
    }

    return $null
}

function Ensure-ServerVenv {
    $python = Get-ServerPython
    if ($python) {
        Write-Host "Python environment found: $python"
        return $python
    }

    if ($NoInstall) {
        Write-Host "Python venv missing. Run without -NoInstall to create it automatically." -ForegroundColor Yellow
        return $null
    }

    $basePython = Find-BasePython
    if (!$basePython) {
        Write-Host "Python missing. Install Python 3.11+ or set PYTHON to python.exe, then rerun this script." -ForegroundColor Yellow
        return $null
    }

    Write-Host "Creating server venv: $ServerVenvDir"
    Ensure-Directory (Split-Path -Parent $ServerVenvDir)
    & $basePython -m venv $ServerVenvDir
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to create server venv: $ServerVenvDir"
    }

    return Get-ServerPython
}

function Ensure-PythonPackages {
    param([string]$PythonPath)

    if ([string]::IsNullOrWhiteSpace($PythonPath)) {
        return
    }

    $requirements = Join-Path $ServerDir "requirements.txt"
    if (!(Test-Path -LiteralPath $requirements)) {
        throw "requirements.txt not found: $requirements"
    }

    & $PythonPath -c "import flask, requests, dotenv" *> $null
    $basicOk = $LASTEXITCODE -eq 0

    & $PythonPath -c "import funasr, modelscope, torch, torchaudio, transformers" *> $null
    $voiceOk = $LASTEXITCODE -eq 0

    if ($basicOk -and $voiceOk) {
        Write-Host "Python packages OK."
        return
    }

    if ($NoInstall) {
        Write-Host "Python packages missing. Run without -NoInstall to install requirements.txt." -ForegroundColor Yellow
        return
    }

    if ($UseChinaMirror) {
        $env:PIP_INDEX_URL = "https://pypi.tuna.tsinghua.edu.cn/simple"
        $env:PIP_TRUSTED_HOST = "pypi.tuna.tsinghua.edu.cn"
    }

    Write-Host "Installing Python packages from requirements.txt..."
    & $PythonPath -m pip install -r $requirements
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to install Python packages."
    }
}

function Ensure-AppDependencies {
    if (!(Test-Path -LiteralPath (Join-Path $AppDir "package.json"))) {
        Write-Host "App package.json not found, skipped npm check."
        return
    }

    if (Test-Path -LiteralPath (Join-Path $AppDir "node_modules")) {
        Write-Host "App node_modules OK."
        return
    }

    if ($NoInstall -or $SkipNode) {
        Write-Host "App node_modules missing. Run without -NoInstall/-SkipNode to install it." -ForegroundColor Yellow
        return
    }

    $npm = Get-Command npm -ErrorAction SilentlyContinue
    if (!$npm) {
        Write-Host "npm missing. Install Node.js, then rerun this script." -ForegroundColor Yellow
        return
    }

    if ($UseChinaMirror) {
        # 这行很重要：只影响当前安装命令，不污染系统级 npm 配置。
        $env:NPM_CONFIG_REGISTRY = "https://registry.npmmirror.com"
    }

    Push-Location $AppDir
    try {
        if (Test-Path -LiteralPath "package-lock.json") {
            npm ci
        } else {
            npm install
        }
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to install App npm dependencies."
        }
    } finally {
        Pop-Location
    }
}

function Test-AndroidTools {
    if ($SkipAndroidCheck) {
        return
    }

    if ([string]::IsNullOrWhiteSpace($env:JAVA_HOME) -and !(Get-Command java -ErrorAction SilentlyContinue)) {
        Write-Host "Java/JDK missing. Android build needs JDK 11+ or Android Studio JBR." -ForegroundColor Yellow
    }

    if ([string]::IsNullOrWhiteSpace($env:ANDROID_HOME) -and [string]::IsNullOrWhiteSpace($env:ANDROID_SDK_ROOT)) {
        Write-Host "Android SDK missing. Set ANDROID_HOME or ANDROID_SDK_ROOT before building APK." -ForegroundColor Yellow
    }
}

Write-Step "配置服务端本地依赖目录"
Ensure-Directory $DepsDir
Ensure-Directory (Join-Path $DepsDir "pip-cache")
Ensure-Directory (Join-Path $DepsDir "huggingface")
Ensure-Directory (Join-Path $DepsDir "modelscope")
Ensure-Directory (Join-Path $DepsDir "torch")
Ensure-Directory (Join-Path $DepsDir "cache")
Ensure-Directory (Join-Path $DepsDir "npm-cache")
Ensure-Directory (Join-Path $DepsDir "gradle")
Ensure-Directory (Join-Path $ServerDir "work")
Ensure-Directory (Join-Path $ServerDir "work\cache")

Set-ProcessEnvPath "BIRD_PROJECT_ROOT" $ProjectRoot
Set-ProcessEnvPath "BIRD_DEPS_DIR" $DepsDir
Set-ProcessEnvPath "BIRD_SERVER_VENV_DIR" $ServerVenvDir
Set-ProcessEnvPath "PIP_CACHE_DIR" (Join-Path $DepsDir "pip-cache")
Set-ProcessEnvPath "HF_HOME" (Join-Path $DepsDir "huggingface")
Set-ProcessEnvPath "HF_HUB_CACHE" (Join-Path $DepsDir "huggingface\hub")
Set-ProcessEnvPath "TRANSFORMERS_CACHE" (Join-Path $DepsDir "huggingface\transformers")
Set-ProcessEnvPath "MODELSCOPE_CACHE" (Join-Path $DepsDir "modelscope")
Set-ProcessEnvPath "TORCH_HOME" (Join-Path $DepsDir "torch")
Set-ProcessEnvPath "XDG_CACHE_HOME" (Join-Path $DepsDir "cache")
Set-ProcessEnvPath "PYTHONPYCACHEPREFIX" (Join-Path $DepsDir "pycache")
Set-ProcessEnvPath "NPM_CONFIG_CACHE" (Join-Path $DepsDir "npm-cache")
Set-ProcessEnvPath "NPM_CONFIG_PREFIX" (Join-Path $DepsDir "npm-global")
Set-ProcessEnvPath "GRADLE_USER_HOME" (Join-Path $DepsDir "gradle")
Set-ProcessEnvPath "ANDROID_USER_HOME" (Join-Path $DepsDir "android-home")

Write-Step "同步服务端 .env"
$envExample = Join-Path $ServerDir ".env.example"
$envFile = Join-Path $ServerDir ".env"
if (!(Test-Path -LiteralPath $envFile)) {
    if (!(Test-Path -LiteralPath $envExample)) {
        throw ".env.example not found: $envExample"
    }
    Copy-Item -LiteralPath $envExample -Destination $envFile
}

Set-DotEnvValue $envFile "BIRD_PROJECT_ROOT" "../.."
Set-DotEnvValue $envFile "BIRD_MOBILE_APP_DIR" "smart-bird-server/App"
Set-DotEnvValue $envFile "BIRD_SERVER_WORK_DIR" "smart-bird-server/server/work"
Set-DotEnvValue $envFile "BIRD_SERVER_CACHE_DIR" "smart-bird-server/server/work/cache"
Set-DotEnvValue $envFile "BIRD_HISTORY_PATH" "smart-bird-server/server/work/sensor_history.json"
Set-DotEnvValue $envFile "BIRD_DB_PATH" "smart-bird-server/server/work/bird_history.db"
Set-DotEnvValue $envFile "BIRD_DEPS_DIR" "smart-bird-server/.deps"
Set-DotEnvValue $envFile "BIRD_SERVER_VENV_DIR" "smart-bird-server/.deps/python/server"
Set-DotEnvValue $envFile "PIP_CACHE_DIR" "smart-bird-server/.deps/pip-cache"
Set-DotEnvValue $envFile "HF_HOME" "smart-bird-server/.deps/huggingface"
Set-DotEnvValue $envFile "HF_HUB_CACHE" "smart-bird-server/.deps/huggingface/hub"
Set-DotEnvValue $envFile "TRANSFORMERS_CACHE" "smart-bird-server/.deps/huggingface/transformers"
Set-DotEnvValue $envFile "MODELSCOPE_CACHE" "smart-bird-server/.deps/modelscope"
Set-DotEnvValue $envFile "TORCH_HOME" "smart-bird-server/.deps/torch"
Set-DotEnvValue $envFile "XDG_CACHE_HOME" "smart-bird-server/.deps/cache"

if (!$SkipPython) {
    Write-Step "检查 Python 服务端环境"
    $serverPython = Ensure-ServerVenv
    Ensure-PythonPackages $serverPython
}

Write-Step "检查 App 环境"
Ensure-AppDependencies
Test-AndroidTools

Write-Step "完成"
Write-Host "服务端依赖目录: $DepsDir"
Write-Host "启动服务端:"
Write-Host "  cd `"$ServerRoot`""
Write-Host "  .\run_server.ps1"
