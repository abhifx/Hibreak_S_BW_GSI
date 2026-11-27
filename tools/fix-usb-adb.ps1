# fix-usb-adb.ps1 -- release the HiBreak from usbipd so Windows can use USB adb.
#
#   Elevated PowerShell:
#     pwsh -File tools/fix-usb-adb.ps1            # unbind/shared -> Windows USB adb
#     pwsh -File tools/fix-usb-adb.ps1 -Share     # bind again for WSL/usbip use
#
# Why: the HiBreak is often left as `Shared (forced)` in `usbipd list` from the
# WSL work. While it is shared, Windows enumerates only a "USBIP Shared Device"
# (VID_0E8D&PID_201C) and never binds the ADB interface, so `adb devices` shows
# only the TCP entry. Unbinding hands the device back to Windows; nothing on the
# phone, the DSU or the HWC shim is touched.
#
# The script is idempotent: it does nothing if the device is already local, and
# it restarts the adb server so the USB device is picked up immediately.
param(
    [string]$Match  = '0e8d:201c',                 # MTK vendor:product of the HiBreak
    [string]$Serial = 'B531D1W2GFSA006000183',     # its USB serial
    [switch]$Share,
    [switch]$NoAdbRestart
)

$ErrorActionPreference = 'Continue'

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Admin)) {
    Write-Host "Administrator rights are required; relaunching elevated..." -ForegroundColor Yellow
    $pwsh = (Get-Process -Id $PID).Path
    $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath)
    if ($Share) { $argv += '-Share' }
    Start-Process -FilePath $pwsh -Verb RunAs -ArgumentList $argv
    return
}

if (-not (Get-Command usbipd -ErrorAction SilentlyContinue)) {
    throw "usbipd is not installed or not on PATH"
}

function Get-Target {
    foreach ($line in (& usbipd list 2>&1)) {
        if ($line -match "^\s*(\S+)\s+$([regex]::Escape($Match))\s+(.+?)\s*$") {
            return [pscustomobject]@{ BusId = $Matches[1]; Rest = $Matches[2] }
        }
    }
    return $null
}

$t = Get-Target
if (-not $t) {
    Write-Host "HiBreak ($Match) is not plugged in or not visible to usbipd." -ForegroundColor Yellow
    Write-Host "Check the cable/port, then run again."
    exit 1
}

Write-Host "HiBreak on bus $($t.BusId): $($t.Rest)"

if ($Share) {
    if ($t.Rest -match 'Not shared') {
        & usbipd bind --busid $t.BusId
        if ($LASTEXITCODE -ne 0) { throw "usbipd bind failed ($LASTEXITCODE)" }
        Write-Host "Shared again for usbip/WSL. Windows USB adb will not work until 'usbipd unbind'."
    } else {
        Write-Host "Already shared/attached; nothing to do."
    }
    return
}

switch -Regex ($t.Rest) {
    'Shared' {
        & usbipd unbind --busid $t.BusId
        if ($LASTEXITCODE -ne 0) { throw "usbipd unbind failed ($LASTEXITCODE)" }
        Write-Host "Unbound; handing the device back to Windows."
        Start-Sleep -Seconds 2
    }
    'Attached' {
        & usbipd detach --busid $t.BusId
        if ($LASTEXITCODE -ne 0) { throw "usbipd detach failed ($LASTEXITCODE)" }
        Write-Host "Detached from the usbip client."
        Start-Sleep -Seconds 2
    }
    default {
        Write-Host "Already bound to Windows; no usbip change needed."
    }
}

if (-not $NoAdbRestart) {
    try { & adb kill-server 2>$null | Out-Null } catch { }
    Start-Sleep -Seconds 1
    try { & adb start-server 2>$null | Out-Null } catch { }
}

Write-Host "Waiting for the USB adb device (up to 25 s)..."
$found = $null
for ($i = 0; $i -lt 25; $i++) {
    Start-Sleep -Seconds 1
    $devs = @(& adb devices -l 2>$null)
    $found = $devs | Where-Object { $_ -match [regex]::Escape($Serial) -and $_ -match '\sdevice\s' }
    if ($found) { break }
    if ($i -eq 4) {
        Write-Host "  still waiting; if it stays absent, unplug/replug the cable once." -ForegroundColor DarkGray
    }
}

Write-Host ""
Write-Host "usbipd:"
& usbipd list 2>&1 | Select-String -Pattern $Match | ForEach-Object { "  $_" }
Write-Host "adb:"
& adb devices -l 2>$null | ForEach-Object { "  $_" }

if ($found) {
    Write-Host ""
    Write-Host "USB adb is up: $Serial" -ForegroundColor Green
    Write-Host "Use it as the deploy serial:"
    Write-Host "  pwsh -File build\hwc-shim-mt6765\deploy.ps1 -Serial $Serial -Fastconv 1 -Skipcopy 1 -Novsync 1 -Log 1"
} else {
    Write-Host ""
    Write-Host "The device did not appear over USB." -ForegroundColor Yellow
    Write-Host "Phone side is already correct (sys.usb.config=adb). Check, in order:"
    Write-Host "  1. a data cable (not charge-only) and a port directly on the PC"
    Write-Host "  2. On the phone: unlock, accept the 'Allow USB debugging' prompt"
    Write-Host "  3. Developer options -> USB debugging = on; USB mode = File transfer"
    exit 2
}
