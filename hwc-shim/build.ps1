# Build hwcshim_mt6765 -> out\hwcomposer.mt6765.so (arm64, no libc++ dependency).
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
$so = Join-Path $out 'hwcomposer.mt6765.so'

Write-Host "NDK : $Ndk"
Write-Host "CXX : $cxx"
Write-Host "OUT : $so"

& $cxx --target=aarch64-linux-android$Api -shared -fPIC -O2 -g0 `
    -fno-exceptions -fno-rtti -nostdlib++ `
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers `
    -o $so (Join-Path $here 'hwcshim_mt6765.cpp') `
    -llog -ldl
if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }

& (Join-Path $bin 'llvm-readelf.exe') -d $so | Select-String 'NEEDED'
& (Join-Path $bin 'llvm-readelf.exe') -sW --dyn-syms $so | Select-String 'HMI|mtk_commit'
Get-Item $so | Select-Object Name, Length, LastWriteTime
