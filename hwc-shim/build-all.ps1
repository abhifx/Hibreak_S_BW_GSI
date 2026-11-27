param(
    [string]$Ndk = $env:ANDROID_NDK_HOME,
    [int]$Api = 30
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
& (Join-Path $here 'gray-helper\build.ps1') -Ndk $Ndk -Api $Api
if ($LASTEXITCODE -ne 0) { throw "gray-helper build failed" }
& (Join-Path $here 'build.ps1') -Ndk $Ndk -Api $Api
if ($LASTEXITCODE -ne 0) { throw "HWC shim build failed" }
& (Join-Path $here 'gray-helper-test\build.ps1') -Ndk $Ndk -Api $Api
if ($LASTEXITCODE -ne 0) { throw "helper tests build failed" }
Write-Host "All MT6765 helper/shim/test artifacts built."
