# fix-usb-adb-win.ps1 -- make Windows' generic WinUSB binding usable by adb.
#
#   Elevated (or via fix-usb-adb-win.bat, which self-elevates):
#     pwsh -File tools\fix-usb-adb-win.ps1
#
# Why: after usbipd releases the HiBreak, Windows binds it to the generic WinUSB
# driver (Class FF/42/01, the Android ADB interface). The generic binding
# registers the phone's own MS-OS-descriptor GUID, while adb enumerates by the
# Android ADB GUID {F72FE0D4-CBCB-407d-8814-9ED673D0DD6B}. This writes that GUID
# into the device's "Device Parameters" key and re-enumerates the device.
param(
    [string]$HardwareId = 'USB\VID_0E8D&PID_201C',
    [string]$Serial = 'B531D1W2GFSA006000183',
    [switch]$NoAdbRestart
)

$ErrorActionPreference = 'Continue'
$AdbGuid = '{F72FE0D4-CBCB-407d-8814-9ED673D0DD6B}'

function Say($m) { Write-Host $m }
function Warn($m) { Write-Host $m -ForegroundColor Yellow }
function Fail($m) { Write-Host $m -ForegroundColor Red }

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Admin)) {
    Fail "This script must run as Administrator."
    Fail "Run tools\fix-usb-adb-win.bat and accept the UAC prompt."
    exit 1
}

Say "Looking for $HardwareId ..."
$devs = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
          Where-Object { $_.InstanceId -like "$HardwareId*" })
if ($devs.Count -eq 0) {
    Fail "Device not present. Plug the USB cable in (data cable, direct port) and rerun."
    exit 1
}

$dev = $devs | Where-Object { $_.Service -eq 'WinUSB' } | Select-Object -First 1
if (-not $dev) { $dev = $devs | Select-Object -First 1 }
Say "device : $($dev.InstanceId)"
Say "service: $($dev.Service)   status: $($dev.Status)"

$key = "HKLM:\SYSTEM\CurrentControlSet\Enum\$($dev.InstanceId)\Device Parameters"
if (-not (Test-Path $key)) {
    Warn "Key missing, creating: $key"
    try { New-Item -Path $key -Force -ErrorAction Stop | Out-Null }
    catch { Fail "Cannot create the key: $($_.Exception.Message)"; exit 1 }
}

$current = $null
try { $current = (Get-ItemProperty -Path $key -Name DeviceInterfaceGUIDs -ErrorAction Stop).DeviceInterfaceGUIDs }
catch { }

if ($current -and ($current -contains $AdbGuid -or "$current" -eq $AdbGuid)) {
    Say "ADB interface GUID already present."
} else {
    Say "Current GUID(s): $(if ($current) { $current -join ', ' } else { '<none>' })"
    try {
        New-ItemProperty -Path $key -Name DeviceInterfaceGUIDs -PropertyType MultiString `
            -Value @($AdbGuid) -Force -ErrorAction Stop | Out-Null
        Say "Wrote ADB interface GUID."
    } catch {
        Fail "Registry write failed: $($_.Exception.Message)"
        Fail "The device key is owned by SYSTEM; this usually needs the INF route instead."
        exit 1
    }
}

# Re-enumerate. pnputil exists on every supported build; the PnpDevice cmdlets do
# not always, so prefer pnputil and fall back.
Say "Re-enumerating the device..."
$restarted = $false
try {
    & pnputil /restart-device "$($dev.InstanceId)" 2>&1 | ForEach-Object { "  $_" }
    if ($LASTEXITCODE -eq 0) { $restarted = $true }
} catch { }
if (-not $restarted) {
    try {
        Disable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false -ErrorAction Stop
        Start-Sleep -Seconds 2
        Enable-PnpDevice -InstanceId $dev.InstanceId -Confirm:$false -ErrorAction Stop
        $restarted = $true
    } catch {
        Warn "Could not re-enumerate automatically: $($_.Exception.Message)"
        Warn "Unplug and replug the USB cable once."
    }
}

if (-not $NoAdbRestart) {
    try { & adb kill-server 2>$null | Out-Null } catch { }
    Start-Sleep -Seconds 1
    try { & adb start-server 2>$null | Out-Null } catch { }
}

Say "Waiting for the USB adb device (up to 25 s)..."
$found = $null
for ($i = 0; $i -lt 25; $i++) {
    Start-Sleep -Seconds 1
    $found = @(& adb devices -l 2>$null) | Where-Object {
        $_ -match [regex]::Escape($Serial) -and $_ -match '\sdevice\s'
    }
    if ($found) { break }
    if ($i -eq 5) { Warn "  still waiting; if it stays absent, unplug/replug once." }
}

Say ""
Say "adb:"
& adb devices -l 2>$null | ForEach-Object { "  $_" }

if ($found) {
    Say ""
    Say "USB adb is up: $Serial"
    Say "Use it as the deploy serial:"
    Say "  pwsh -File build\hwc-shim-mt6765\deploy.ps1 -Serial $Serial -Fast 1 -Ownbuf 1 -VendHz 2 -FastIdle 0 -Log 1"
    exit 0
}

Fail ""
Fail "Still not visible to adb."
Fail "Next options, in order:"
Fail "  1. unplug/replug the cable, then rerun this script"
Fail "  2. enable Developer options -> Wireless debugging on the phone (Wi-Fi)"
exit 2
