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
$exe = Join-Path $out 'gray_helper_client'
Write-Host "CXX : $cxx"
Write-Host "OUT : $exe"
& $cxx --target=aarch64-linux-android$Api -std=c++17 -O2 -g0 -DNDEBUG `
    -fPIE -fno-exceptions -fno-rtti -nostdlib++ `
    -Wall -Wextra -Wpedantic `
    -o $exe (Join-Path $here 'gray_helper_client.cpp') `
    -lEGL -lGLESv2 -lnativewindow -landroid -llog
if ($LASTEXITCODE -ne 0) { throw "compile failed ($LASTEXITCODE)" }
& (Join-Path $bin 'llvm-readelf.exe') -h $exe | Select-String 'Class:|Machine:|Type:'
& (Join-Path $bin 'llvm-readelf.exe') -d $exe | Select-String 'NEEDED'
Get-FileHash -Algorithm SHA256 $exe | Select-Object Algorithm, Hash, Path

$dump = Join-Path $out 'gbfmt_dump'
& $cxx --target=aarch64-linux-android$Api -std=c++17 -O2 -g0 -DNDEBUG `
    -fPIE -fno-exceptions -fno-rtti -nostdlib++ `
    -Wall -Wextra -Wpedantic `
    -o $dump (Join-Path $here 'gbfmt_dump.cpp') `
    -lnativewindow -landroid -llog
if ($LASTEXITCODE -ne 0) { throw "gbfmt_dump compile failed ($LASTEXITCODE)" }
Get-FileHash -Algorithm SHA256 $dump | Select-Object Algorithm, Hash, Path
