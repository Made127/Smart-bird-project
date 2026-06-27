param(
    [switch]$NoInstall,
    [switch]$Build,
    [string]$Port = $env:IDF_PORT,
    [string]$EspIdfVersion = "v5.1.2"
)

$ErrorActionPreference = "Stop"

# ESP32 工程根目录由脚本位置确定，整目录迁移后不需要改绝对路径。
$EspRoot = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($EspRoot)) {
    $EspRoot = Split-Path -Parent $PSCommandPath
}
$DepsDir = Join-Path $EspRoot ".deps"
$LocalIdfPath = Join-Path $DepsDir "esp-idf"
$LocalIdfToolsPath = Join-Path $DepsDir "espressif"

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

function Find-Python {
    if (-not ([string]::IsNullOrWhiteSpace($env:IDF_PYTHON)) -and (Test-PythonWorks $env:IDF_PYTHON)) {
        return $env:IDF_PYTHON
    }

    if (-not ([string]::IsNullOrWhiteSpace($env:PYTHON)) -and (Test-PythonWorks $env:PYTHON)) {
        return $env:PYTHON
    }

    $pyCommand = Get-Command py -ErrorAction SilentlyContinue
    if ($pyCommand) {
        return $pyCommand.Source
    }

    foreach ($command in @(Get-Command python -ErrorAction SilentlyContinue)) {
        # 这行很重要：WindowsApps 的 python.exe 通常只是商店占位符，不能稳定运行 ESP-IDF 工具链。
        if ($command -and $command.Source -notlike "*\WindowsApps\python.exe") {
            return $command.Source
        }
    }

    return $null
}

function Clone-EspIdf {
    if ($NoInstall) {
        Write-Host "ESP-IDF missing. Run without -NoInstall to clone it into: $LocalIdfPath" -ForegroundColor Yellow
        return
    }

    $git = Get-Command git -ErrorAction SilentlyContinue
    if (!$git) {
        Write-Host "Git missing. Install Git, then rerun this script to auto-clone ESP-IDF." -ForegroundColor Yellow
        return
    }

    Write-Host "Cloning ESP-IDF $EspIdfVersion to: $LocalIdfPath"
    & $git.Source clone --recursive --branch $EspIdfVersion https://github.com/espressif/esp-idf.git $LocalIdfPath
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to clone ESP-IDF."
    }
}

function Ensure-EspIdfTools {
    param([string]$IdfPath)

    $installPs1 = Join-Path $IdfPath "install.ps1"
    if (!(Test-Path -LiteralPath $installPs1)) {
        Write-Host "install.ps1 not found under ESP-IDF: $IdfPath" -ForegroundColor Yellow
        return
    }

    $pythonEnvDir = Join-Path $LocalIdfToolsPath "python_env"
    if ((Test-Path -LiteralPath $pythonEnvDir) -or $NoInstall) {
        if ($NoInstall -and !(Test-Path -LiteralPath $pythonEnvDir)) {
            Write-Host "ESP-IDF tools missing. Run without -NoInstall to install them." -ForegroundColor Yellow
        } else {
            Write-Host "ESP-IDF tools path OK: $LocalIdfToolsPath"
        }
        return
    }

    Write-Host "Installing ESP-IDF tools for esp32s3..."
    Push-Location $IdfPath
    try {
        # 这行很重要：install.ps1 会读取 IDF_TOOLS_PATH，因此工具链会安装到 smart-bird-EPS32\.deps。
        & $installPs1 esp32s3
        if ($LASTEXITCODE -ne 0) {
            throw "ESP-IDF tools installation failed."
        }
    } finally {
        Pop-Location
    }
}

Write-Step "配置 ESP32 本地依赖目录"
Ensure-Directory $DepsDir
Ensure-Directory $LocalIdfToolsPath

$env:BIRD_TEST_HOME = $EspRoot
$env:BIRD_EPS32_DEPS_DIR = $DepsDir
$env:IDF_TOOLS_PATH = $LocalIdfToolsPath
if ([string]::IsNullOrWhiteSpace($env:IDF_TARGET)) {
    $env:IDF_TARGET = "esp32s3"
}
if ([string]::IsNullOrWhiteSpace($Port)) {
    $Port = "COM7"
}
$env:IDF_PORT = $Port

if (Test-Path -LiteralPath $LocalIdfPath) {
    $env:IDF_PATH = $LocalIdfPath
} elseif ([string]::IsNullOrWhiteSpace($env:IDF_PATH) -or !(Test-Path -LiteralPath $env:IDF_PATH)) {
    Clone-EspIdf
    if (Test-Path -LiteralPath $LocalIdfPath) {
        $env:IDF_PATH = $LocalIdfPath
    }
}

$python = Find-Python
if ($python) {
    $env:IDF_PYTHON = $python
    Write-Host "Python for ESP-IDF: $python"
} else {
    Write-Host "Python missing. Install Python 3.8+ or set IDF_PYTHON/PYTHON." -ForegroundColor Yellow
}

if ([string]::IsNullOrWhiteSpace($env:IDF_PATH) -or !(Test-Path -LiteralPath $env:IDF_PATH)) {
    Write-Host "ESP-IDF not ready. Expected local path: $LocalIdfPath" -ForegroundColor Yellow
    Write-Host "You can also set IDF_PATH to an existing ESP-IDF directory."
    return
}

$idfPy = Join-Path $env:IDF_PATH "tools\idf.py"
if (!(Test-Path -LiteralPath $idfPy)) {
    throw "idf.py not found: $idfPy"
}

Write-Step "检查 ESP-IDF tools"
Ensure-EspIdfTools $env:IDF_PATH

if ($Build) {
    if (!$python) {
        throw "Cannot build because Python is missing."
    }

    Write-Step "执行 ESP32 编译自检"
    Push-Location $EspRoot
    try {
        & $python $idfPy build
        if ($LASTEXITCODE -ne 0) {
            throw "ESP32 build failed."
        }
    } finally {
        Pop-Location
    }
}

Write-Step "完成"
Write-Host "ESP32 依赖目录: $DepsDir"
Write-Host "IDF_PATH=$env:IDF_PATH"
Write-Host "IDF_TOOLS_PATH=$env:IDF_TOOLS_PATH"
Write-Host "监视串口:"
Write-Host "  .\run_monitor.ps1 -Port $Port"
