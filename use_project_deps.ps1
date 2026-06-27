$ErrorActionPreference = "Stop"

# 项目根目录由本脚本位置确定，移动整个目录后不需要手动改绝对路径。
$ProjectRoot = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    # 这行很重要：dot-source 加载时 PSScriptRoot 为空就回退到 PSCommandPath。
    $ProjectRoot = Split-Path -Parent $PSCommandPath
}
if ([string]::IsNullOrWhiteSpace($ProjectRoot) -and $MyInvocation.MyCommand.Path) {
    $ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
}
if ([string]::IsNullOrWhiteSpace($ProjectRoot) -and $MyInvocation.MyCommand.Definition -and (Test-Path -LiteralPath $MyInvocation.MyCommand.Definition)) {
    $ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Definition
}
if ([string]::IsNullOrWhiteSpace($ProjectRoot)) {
    $ProjectRoot = (Get-Location).Path
}

function Set-EnvPathDefault {
    param(
        [string]$Name,
        [string]$Path,
        [switch]$ForceProject
    )

    $keepSystemDeps = ($env:BIRD_KEEP_SYSTEM_DEPS -eq "1")
    $currentValue = [Environment]::GetEnvironmentVariable($Name, "Process")
    if ($ForceProject -and !$keepSystemDeps) {
        # 这行很重要：依赖安装/缓存路径默认强制放到项目内，保证整目录迁移时仍可复用。
        $resolvedPath = Resolve-PathOrCreateParent $Path
        [Environment]::SetEnvironmentVariable($Name, $resolvedPath, "Process")
        Set-Item -Path ("Env:\" + $Name) -Value $resolvedPath
    } elseif (($null -eq $currentValue) -or ($currentValue.Trim().Length -eq 0)) {
        # 未要求强制项目内路径时，只设置缺失的默认值，不覆盖用户已经显式配置的环境变量。
        $resolvedPath = Resolve-PathOrCreateParent $Path
        [Environment]::SetEnvironmentVariable($Name, $resolvedPath, "Process")
        Set-Item -Path ("Env:\" + $Name) -Value $resolvedPath
    }
}

function Resolve-PathOrCreateParent {
    param(
        [string]$Path
    )

    $expanded = [Environment]::ExpandEnvironmentVariables($Path)
    if (-not ([System.IO.Path]::IsPathRooted($expanded))) {
        $expanded = Join-Path $ProjectRoot $expanded
    }

    $parent = Split-Path -Parent $expanded
    if ((-not ([string]::IsNullOrWhiteSpace($parent))) -and !(Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }

    return [System.IO.Path]::GetFullPath($expanded)
}

function Ensure-Directory {
    param(
        [string]$Path
    )

    if (!(Test-Path -LiteralPath $Path)) {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
    }
}

Set-EnvPathDefault "BIRD_PROJECT_ROOT" $ProjectRoot
Set-EnvPathDefault "BIRD_TEST_HOME" (Join-Path $ProjectRoot "smart-bird-EPS32")

$ServerRoot = Join-Path $ProjectRoot "smart-bird-server"
$EspRoot = Join-Path $ProjectRoot "smart-bird-EPS32"
$ServerDepsDir = Join-Path $ServerRoot ".deps"
$EspDepsDir = Join-Path $EspRoot ".deps"

Set-EnvPathDefault "BIRD_DEPS_DIR" $ServerDepsDir -ForceProject
Set-EnvPathDefault "BIRD_EPS32_DEPS_DIR" $EspDepsDir -ForceProject

$DepsDir = $env:BIRD_DEPS_DIR
if ([string]::IsNullOrWhiteSpace($DepsDir)) {
    $DepsDir = $ServerDepsDir
    $env:BIRD_DEPS_DIR = $DepsDir
}
Ensure-Directory $DepsDir
Ensure-Directory $EspDepsDir

# Python、模型、npm、Gradle 默认放到 smart-bird-server\.deps，服务端目录可以单独迁移。
Set-EnvPathDefault "BIRD_SERVER_VENV_DIR" (Join-Path $DepsDir "python\server") -ForceProject
Set-EnvPathDefault "PIP_CACHE_DIR" (Join-Path $DepsDir "pip-cache") -ForceProject
Set-EnvPathDefault "HF_HOME" (Join-Path $DepsDir "huggingface") -ForceProject
Set-EnvPathDefault "HF_HUB_CACHE" (Join-Path $DepsDir "huggingface\hub") -ForceProject
Set-EnvPathDefault "TRANSFORMERS_CACHE" (Join-Path $DepsDir "huggingface\transformers") -ForceProject
Set-EnvPathDefault "MODELSCOPE_CACHE" (Join-Path $DepsDir "modelscope") -ForceProject
Set-EnvPathDefault "TORCH_HOME" (Join-Path $DepsDir "torch") -ForceProject
Set-EnvPathDefault "XDG_CACHE_HOME" (Join-Path $DepsDir "cache") -ForceProject
Set-EnvPathDefault "PYTHONPYCACHEPREFIX" (Join-Path $DepsDir "pycache") -ForceProject
Set-EnvPathDefault "NPM_CONFIG_CACHE" (Join-Path $DepsDir "npm-cache") -ForceProject
Set-EnvPathDefault "NPM_CONFIG_PREFIX" (Join-Path $DepsDir "npm-global") -ForceProject
Set-EnvPathDefault "GRADLE_USER_HOME" (Join-Path $DepsDir "gradle") -ForceProject
Set-EnvPathDefault "ANDROID_USER_HOME" (Join-Path $DepsDir "android-home") -ForceProject

# ESP-IDF 本体和工具链默认放到 smart-bird-EPS32\.deps，硬件工程可以单独迁移。
Set-EnvPathDefault "IDF_TOOLS_PATH" (Join-Path $EspDepsDir "espressif") -ForceProject

if ([string]::IsNullOrWhiteSpace($env:IDF_PATH)) {
    # 这行很重要：ESP-IDF 本体默认也指向项目内目录，未安装时错误信息会直接指向这个位置。
    $env:IDF_PATH = Join-Path $EspDepsDir "esp-idf"
}

if ([string]::IsNullOrWhiteSpace($env:IDF_PYTHON)) {
    $idfPython = Get-ChildItem -LiteralPath (Join-Path $EspDepsDir "espressif\python_env") -Filter "python.exe" -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like "*\Scripts\python.exe" } |
        Select-Object -First 1
    if ($idfPython) {
        $env:IDF_PYTHON = $idfPython.FullName
    }
}

if ([string]::IsNullOrWhiteSpace($env:FFMPEG_PATH)) {
    $projectFfmpeg = Join-Path $ProjectRoot "FormatFactory\ffmpeg.exe"
    if (Test-Path -LiteralPath $projectFfmpeg) {
        $env:FFMPEG_PATH = $projectFfmpeg
    }
}





