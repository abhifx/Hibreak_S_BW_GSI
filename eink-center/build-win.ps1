# build-eink-center.ps1 -- build + sign the E-ink Center APK on Windows.
#
#   pwsh -File build-eink-center.ps1
#
# Uses the installed Android SDK build-tools and JDK; signs with the repo's
# debug.keystore so the APK updates the installed app in place.
$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sdk  = Join-Path $env:LOCALAPPDATA 'Android\Sdk'
$bt   = Join-Path $sdk 'build-tools\36.1.0'
$jar  = Join-Path $sdk 'platforms\android-36\android.jar'
if (-not (Test-Path $bt))  { $bt = (Get-ChildItem (Join-Path $sdk 'build-tools') -Directory | Sort-Object Name | Select-Object -Last 1).FullName }
if (-not (Test-Path $jar)) { $jar = (Get-ChildItem (Join-Path $sdk 'platforms') -Directory | Sort-Object Name | Select-Object -Last 1).FullName + '\android.jar' }

$src = Join-Path $here 'src'
$out = Join-Path $here 'build-win'
$ks  = Join-Path $here 'debug.keystore'

Write-Host "build-tools : $bt"
Write-Host "android.jar : $jar"

Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $out 'classes'), (Join-Path $out 'dex') | Out-Null

Write-Host "==> javac"
$sources = Get-ChildItem $src -Recurse -Filter *.java | ForEach-Object { $_.FullName }
& javac --release 11 -nowarn -classpath $jar -d (Join-Path $out 'classes') @sources
if ($LASTEXITCODE -ne 0) { throw "javac failed" }

Write-Host "==> d8"
$classes = Get-ChildItem (Join-Path $out 'classes') -Recurse -Filter *.class | ForEach-Object { $_.FullName }
& (Join-Path $bt 'd8.bat') --lib $jar --min-api 30 --output (Join-Path $out 'dex') @classes
if ($LASTEXITCODE -ne 0) { throw "d8 failed" }

Write-Host "==> aapt2 link"
& (Join-Path $bt 'aapt2.exe') link -o (Join-Path $out 'base.apk') -I $jar `
    --manifest (Join-Path $here 'AndroidManifest.xml') `
    --min-sdk-version 30 --target-sdk-version 34
if ($LASTEXITCODE -ne 0) { throw "aapt2 failed" }

Write-Host "==> add dex"
& jar uf (Join-Path $out 'base.apk') -C (Join-Path $out 'dex') classes.dex
if ($LASTEXITCODE -ne 0) { throw "jar uf failed" }

Write-Host "==> zipalign + sign"
& (Join-Path $bt 'zipalign.exe') -f 4 (Join-Path $out 'base.apk') (Join-Path $out 'aligned.apk')
if ($LASTEXITCODE -ne 0) { throw "zipalign failed" }
& (Join-Path $bt 'apksigner.bat') sign --ks $ks --ks-pass pass:android --key-pass pass:android `
    --out (Join-Path $here 'eink-center.apk') (Join-Path $out 'aligned.apk')
if ($LASTEXITCODE -ne 0) { throw "apksigner failed" }

Get-Item (Join-Path $here 'eink-center.apk') | Select-Object FullName, Length, LastWriteTime
