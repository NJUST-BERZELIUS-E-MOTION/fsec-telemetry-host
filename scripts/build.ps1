# =====================================================================
# build.ps1 - SerialJsonHost 一键构建/运行脚本 (MSVC2022 或 MinGW)
#
# 用法(在 PowerShell 中, 工作目录不限):
#   .\scripts\build.ps1                              # 默认: MSVC Debug
#   .\scripts\build.ps1 -Toolchain msvc  -Config Release
#   .\scripts\build.ps1 -Toolchain mingw -Config Debug
#   .\scripts\build.ps1 -Toolchain msvc  -Config Debug -Run     # 构建后运行
#   .\scripts\build.ps1 -Toolchain msvc  -Config Release -Deploy # 构建+windeployqt
#
# 前置依赖:
#   - Qt 6.11.1  于 D:\Qt6.11\6.11.1 (msvc2022_64 / mingw_64 两套都在)
#   - cmake 于 PATH(系统 4.3.1 已确认可用)
#   - MSVC 套件需要 VS2022 (Community 或 Build Tools), 脚本自动进入其环境
#   - MinGW 套件使用 Qt 自带的 mingw1310_64 与 Ninja, 无需额外环境
# =====================================================================

param(
    [ValidateSet('msvc', 'mingw')] [string]$Toolchain = 'msvc',
    [ValidateSet('Debug', 'Release')] [string]$Config = 'Debug',
    [switch]$Run,
    [switch]$Deploy
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$QtRoot = 'D:\Qt6.11\6.11.1'
$ExeName = 'SerialJsonHost.exe'

function Invoke-Checked {
    param([string]$File, [string[]]$Arguments)
    Write-Host ">> $File $($Arguments -join ' ')" -ForegroundColor Cyan
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code $LASTEXITCODE : $File $($Arguments -join ' ')"
    }
}

switch ($Toolchain) {
    'msvc' {
        $QtDir = Join-Path $QtRoot 'msvc2022_64'
        $BuildDir = Join-Path $Root 'build\msvc'
        $ExeDir = Join-Path $BuildDir $Config
        $ConfigurePreset = 'msvc'
    }
    'mingw' {
        $QtDir = Join-Path $QtRoot 'mingw_64'
        if ($Config -eq 'Release') {
            $BuildDir = Join-Path $Root 'build\mingw-release'
            $ConfigurePreset = 'mingw-release'
        } else {
            $BuildDir = Join-Path $Root 'build\mingw'
            $ConfigurePreset = 'mingw'
        }
        $ExeDir = $BuildDir
    }
}
$Exe = Join-Path $ExeDir $ExeName

# ---------------------------------------------------------------
# MSVC: 定位 VS2022 并进入其开发者环境 (提供 cl.exe / rc.exe 等)
# ---------------------------------------------------------------
if ($Toolchain -eq 'msvc') {
    $vsRoot = $null
    foreach ($candidate in @(
        'C:\Program Files\Microsoft Visual Studio\2022\Community',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools')) {
        if (Test-Path (Join-Path $candidate 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')) {
            $vsRoot = $candidate
            break
        }
    }
    if (-not $vsRoot) {
        throw 'VS2022 (Community/Build Tools) not found. Install it or use -Toolchain mingw.'
    }
    Write-Host ">> Enter VS2022 dev environment: $vsRoot" -ForegroundColor Cyan
    Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation `
        -DevCmdArguments '-arch=x64 -host_arch=x64'
}

# ---------------------------------------------------------------
# 1) 配置 (CMakeCache 不存在时才执行)
# ---------------------------------------------------------------
$cacheFile = Join-Path $BuildDir 'CMakeCache.txt'
if (-not (Test-Path $cacheFile)) {
    Invoke-Checked 'cmake' @('--preset', $ConfigurePreset)
} else {
    Write-Host ">> CMake cache exists, skip configure: $BuildDir"
}

# ---------------------------------------------------------------
# 2) 构建
# ---------------------------------------------------------------
if ($Toolchain -eq 'msvc') {
    Invoke-Checked 'cmake' @('--build', $BuildDir, '--config', $Config)
} else {
    if ($Config -eq 'Release') {
        Invoke-Checked 'cmake' @('--build', '--preset', 'mingw-release')
    } else {
        Invoke-Checked 'cmake' @('--build', '--preset', 'mingw')
    }
}

# ---------------------------------------------------------------
# 3) (可选) 部署 Qt 运行库到 exe 目录 (发布用)
# ---------------------------------------------------------------
if ($Deploy) {
    $windeployqt = Join-Path $QtDir 'bin\windeployqt.exe'
    if (Test-Path $windeployqt) {
        Write-Host ">> Deploying Qt runtime libraries..." -ForegroundColor Cyan
        & $windeployqt '--no-translations' '--no-system-d3d-compiler' `
            '--no-opengl-sw' (Join-Path $ExeDir $ExeName) | Out-Host
    } else {
        Write-Warning "windeployqt not found: $windeployqt"
    }
}

# ---------------------------------------------------------------
# 4) (可选) 运行
# ---------------------------------------------------------------
if ($Run) {
    if (-not (Test-Path $Exe)) {
        throw "Executable not found: $Exe"
    }
    # 让程序能加载 Qt DLL
    $env:Path = (Join-Path $QtDir 'bin') + ';' + $env:Path
    Write-Host ">> Running $Exe" -ForegroundColor Cyan
    & $Exe
}
