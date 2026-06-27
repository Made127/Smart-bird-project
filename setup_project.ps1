param(
    [switch]$UseChinaMirror,
    [switch]$SkipPython,
    [switch]$SkipNode,
    [switch]$SkipEspIdfCheck,
    [switch]$SkipAndroidCheck,
    [switch]$NoInstall
)

$ErrorActionPreference = "Stop"

# 项目根目录由脚本所在位置决定，移动整个项目后不用改绝对路径。
$ProjectRoot = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = Split-Path -Parent $PSCommandPath
}

$projectEnv = Join-Path $ProjectRoot "use_project_deps.ps1"
if (-not (Test-Path -LiteralPath $projectEnv)) {
    throw "use_project_deps.ps1 not found: $projectEnv"
}

# 这行很重要：所有依赖安装和缓存路径都先切到服务端/ESP32 各自的 .deps 下。
. $projectEnv

$BirdTestDir = Join-Path $ProjectRoot "smart-bird-EPS32"
$ServerRoot = Join-Path $ProjectRoot "smart-bird-server"
$ServerDir = Join-Path $ServerRoot "server"
$AppDir = Join-Path $ServerRoot "App"

function Write-Step {
    param([string]$Message)
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Test-CommandExists {
    param([string]$Name)
    return $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

function Test-PythonWorks {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path) -or (-not (Test-Path -LiteralPath $Path))) {
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
        # 这行很重要：WindowsApps 里的 python.exe 通常只是商店占位符，不能用来创建 venv。
        if ($command -and $command.Source -notlike "*\WindowsApps\python.exe") {
            $candidates += $command.Source
        }
    }

    foreach ($candidate in $candidates) {
        if (Test-PythonWorks $candidate) {
            return $candidate
        }
    }

    throw "Cannot find a working Python. Install Python or set PYTHON to python.exe."
}

function Get-ServerPython {
    $candidates = @(
        (Join-Path $env:BIRD_SERVER_VENV_DIR "Scripts\python.exe"),
        (Join-Path $env:BIRD_SERVER_VENV_DIR "bin\python.exe")
    )

    foreach ($candidate in $candidates) {
        if (Test-PythonWorks $candidate) {
            return $candidate
        }
    }

    return $null
}

function Ensure-ServerEnvFile {
    $envExample = Join-Path $ServerDir ".env.example"
    $envFile = Join-Path $ServerDir ".env"

    if (-not (Test-Path -LiteralPath $envExample)) {
        throw ".env.example not found: $envExample"
    }

    if (-not (Test-Path -LiteralPath $envFile)) {
        Copy-Item -LiteralPath $envExample -Destination $envFile
        Write-Host "created server .env: $envFile"
    } else {
        Write-Host "server .env already exists: $envFile"
    }
}

function Ensure-ServerVenv {
    $python = Get-ServerPython
    if ($python) {
        Write-Host "server python: $python"
        return $python
    }

    if ($NoInstall) {
        Write-Host "server venv missing, skipped because -NoInstall was used."
        return $null
    }

    $basePython = Find-BasePython
    $venvDir = $env:BIRD_SERVER_VENV_DIR
    Write-Host "creating server venv: $venvDir"
    New-Item -ItemType Directory -Path (Split-Path -Parent $venvDir) -Force | Out-Null
    & $basePython -m venv $venvDir
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to create server venv: $venvDir"
    }

    $python = Get-ServerPython
    if (-not $python) {
        throw "Cannot find python.exe after creating venv: $venvDir"
    }

    return $python
}

function Install-ServerRequirements {
    param([string]$PythonPath)

    if ([string]::IsNullOrWhiteSpace($PythonPath)) {
        return
    }

    if ($UseChinaMirror) {
        # 国内网络慢时使用清华源；只影响当前脚本进程。
        $env:PIP_INDEX_URL = "https://pypi.tuna.tsinghua.edu.cn/simple"
        $env:PIP_TRUSTED_HOST = "pypi.tuna.tsinghua.edu.cn"
    }

    $requirements = Join-Path $ServerDir "requirements.txt"
    if (-not (Test-Path -LiteralPath $requirements)) {
        throw "requirements.txt not found: $requirements"
    }

    Write-Host "upgrading pip in project venv..."
    & $PythonPath -m pip install --upgrade pip
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to upgrade pip."
    }

    Write-Host "installing Python requirements..."
    & $PythonPath -m pip install -r $requirements
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to install Python requirements."
    }
}

function Install-AppDependencies {
    if (-not (Test-Path -LiteralPath (Join-Path $AppDir "package.json"))) {
        Write-Host "App package.json not found, skipped npm install."
        return
    }

    if (-not (Test-CommandExists "npm")) {
        Write-Host "npm not found, skipped App dependencies."
        return
    }

    if ($UseChinaMirror) {
        # 这行很重要：只给当前命令设置 npm 镜像，不污染系统级 npm 配置。
        $env:NPM_CONFIG_REGISTRY = "https://registry.npmmirror.com"
    }

    Push-Location $AppDir
    try {
        if (Test-Path -LiteralPath "package-lock.json") {
            Write-Host "installing App dependencies with npm ci..."
            npm ci
        } else {
            Write-Host "installing App dependencies with npm install..."
            npm install
        }

        if ($LASTEXITCODE -ne 0) {
            throw "Failed to install App npm dependencies."
        }
    } finally {
        Pop-Location
    }
}

function Test-EspIdfEnvironment {
    if ($SkipEspIdfCheck) {
        return
    }

    $idfPy = Join-Path $env:IDF_PATH "tools\idf.py"
    if (Test-Path -LiteralPath $idfPy) {
        Write-Host "ESP-IDF found: $env:IDF_PATH"
    } else {
        Write-Host "ESP-IDF not found at: $env:IDF_PATH"
        Write-Host "Put ESP-IDF in smart-bird-EPS32\.deps\esp-idf or set IDF_PATH before build."
    }

    Write-Host "ESP-IDF tools path: $env:IDF_TOOLS_PATH"
}

function Test-AndroidEnvironment {
    if ($SkipAndroidCheck) {
        return
    }

    if ([string]::IsNullOrWhiteSpace($env:JAVA_HOME) -and (-not (Test-CommandExists "java"))) {
        Write-Host "Java not found. Android build needs JDK 11 or Android Studio JBR."
    } else {
        Write-Host "Java environment detected."
    }

    if ([string]::IsNullOrWhiteSpace($env:ANDROID_HOME) -and [string]::IsNullOrWhiteSpace($env:ANDROID_SDK_ROOT)) {
        Write-Host "Android SDK not configured. Set ANDROID_HOME or ANDROID_SDK_ROOT before APK build."
    } else {
        Write-Host "Android SDK environment detected."
    }
}

Write-Step "配置项目内依赖目录"
Write-Host "BIRD_PROJECT_ROOT=$env:BIRD_PROJECT_ROOT"
Write-Host "BIRD_DEPS_DIR=$env:BIRD_DEPS_DIR"
Write-Host "BIRD_SERVER_VENV_DIR=$env:BIRD_SERVER_VENV_DIR"
Write-Host "PIP_CACHE_DIR=$env:PIP_CACHE_DIR"
Write-Host "MODELSCOPE_CACHE=$env:MODELSCOPE_CACHE"
Write-Host "GRADLE_USER_HOME=$env:GRADLE_USER_HOME"
Write-Host "IDF_TOOLS_PATH=$env:IDF_TOOLS_PATH"

Write-Step "配置服务端 .env"
Ensure-ServerEnvFile

if (-not $SkipPython) {
    Write-Step "配置 Python 服务端依赖"
    $serverPython = Ensure-ServerVenv
    if (-not $NoInstall) {
        Install-ServerRequirements $serverPython
    }
} else {
    Write-Step "跳过 Python 依赖"
}

if (-not $SkipNode) {
    Write-Step "配置 App npm 依赖"
    if ($NoInstall) {
        Write-Host "npm install skipped because -NoInstall was used."
    } else {
        Install-AppDependencies
    }
} else {
    Write-Step "跳过 App npm 依赖"
}

Write-Step "检查 ESP-IDF 和 Android 环境"
Test-EspIdfEnvironment
Test-AndroidEnvironment

Write-Step "完成"
Write-Host "服务端自检："
Write-Host "  cd `"$ServerRoot`""
Write-Host "  .\check_server_env.ps1"
Write-Host ""
Write-Host "ESP32 自检："
Write-Host "  cd `"$BirdTestDir`""
Write-Host "  .\check_esp32_env.ps1"
Write-Host ""
Write-Host "启动服务端："
Write-Host "  cd `"$ServerRoot`""
Write-Host "  .\run_server.ps1"
Write-Host ""
Write-Host "如需国内镜像重新配置："
Write-Host "  .\setup_project.ps1 -UseChinaMirror"


