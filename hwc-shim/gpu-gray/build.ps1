# Build gpu_gray_bench -> out\gpu_gray_bench (arm64, no libc++ dependency).
#
#   pwsh -File build.ps1
#
# NDK: uses $env:ANDROID_NDK_HOME, else the newest $env:LOCALAPPDATA\Android\Sdk\ndk\*.
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
               Sort-Object { [version]($_.Name -replace '[^0-9.].*$','') } |
               Select-Object -Last 1 -ExpandProperty FullName
    }
}
if (-not $Ndk -or -not (Test-Path $Ndk)) { throw "NDK not found; set ANDROID_NDK_HOME" }

$bin = Join-Path $Ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$cxx = Join-Path $bin "aarch64-linux-android$Api-clang++.cmd"
if (-not (Test-Path $cxx)) { $cxx = Join-Path $bin 'clang++.exe' }
if (-not (Test-Path $cxx)) { throw "clang++ not found under $bin" }

$out = Join-Path $here 'out'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$exe = Join-Path $out 'gpu_gray_bench'

Write-Host "NDK : $Ndk"
Write-Host "CXX : $cxx"
Write-Host "OUT : $exe"

& $cxx --target=aarch64-linux-android$Api -O2 -g0 `
    -fno-exceptions -fno-rtti -nostdlib++ `
    -Wall -Wextra -Wno-unused-parameter `
    -o $exe (Join-Path $here 'gpu_gray_bench.cpp') `
    -lEGL -lGLESv2 -landroid -llog -lm
if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }

& (Join-Path $bin 'llvm-readelf.exe') -d $exe | Select-String 'NEEDED'
Get-Item $exe | Select-Object Name, Length, LastWriteTime
