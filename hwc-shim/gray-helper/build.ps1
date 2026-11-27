# Build gray_helper -> out\gray_helper (Android arm64, API 30, no libc++).
#
#   pwsh -File build.ps1
#   pwsh -File build.ps1 -Ndk C:\path\to\android-ndk -Api 30
#
# NDK discovery: $env:ANDROID_NDK_HOME, then the newest NDK under
# $env:LOCALAPPDATA\Android\Sdk\ndk.
param(
    [string]$Ndk = $env:ANDROID_NDK_HOME,
    [int]$Api = 30
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $Ndk) {
    $root = Join-Path $env:LOCALAPPDATA 'Android\Sdk\ndk'
    if (Test-Path $root) {
        $Ndk = Get-ChildItem $root -Directory |
            Sort-Object { [version]($_.Name -replace '[^0-9.].*$', '') } |
            Select-Object -Last 1 -ExpandProperty FullName
    }
}
if (-not $Ndk -or -not (Test-Path $Ndk)) {
    throw "NDK not found; set ANDROID_NDK_HOME or pass -Ndk"
}

$bin = Join-Path $Ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$cxx = Join-Path $bin "aarch64-linux-android$Api-clang++.cmd"
if (-not (Test-Path $cxx)) { $cxx = Join-Path $bin 'clang++.exe' }
if (-not (Test-Path $cxx)) { throw "clang++ not found under $bin" }

$out = Join-Path $here 'out'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$exe = Join-Path $out 'gray_helper'

Write-Host "NDK : $Ndk"
Write-Host "CXX : $cxx"
Write-Host "OUT : $exe"

& $cxx --target=aarch64-linux-android$Api -std=c++17 -O2 -g0 -DNDEBUG `
    -fPIE -fno-exceptions -fno-rtti -nostdlib++ `
    -ffunction-sections -fdata-sections '-Wl,--gc-sections' '-Wl,--build-id=sha1' `
    -Wall -Wextra -Wpedantic -Wno-unused-parameter `
    -o $exe (Join-Path $here 'gray_helper.cpp') `
    -lEGL -lGLESv2 -lnativewindow -landroid -llog
if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }

& (Join-Path $bin 'llvm-readelf.exe') -h $exe | Select-String 'Class:|Machine:|Type:'
& (Join-Path $bin 'llvm-readelf.exe') -d $exe | Select-String 'NEEDED'
Get-FileHash -Algorithm SHA256 $exe | Select-Object Algorithm, Hash, Path
Get-Item $exe | Select-Object Name, Length, LastWriteTime
