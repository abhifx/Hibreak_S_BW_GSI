# Deploy the mt6765 HWC shim on the TD A16 GSI, reversibly.
#
#   pwsh -File deploy.ps1 -Serial 192.168.1.100:5555            # install + restart
#   pwsh -File deploy.ps1 -Serial ... -Nofence 0                 # A/B the fence change
#   pwsh -File deploy.ps1 -Serial ... -Revert                    # umount, back to vendor
#
# What it does (all verified on device 2026-11-20):
#   * copies /vendor/lib64/hw (7.5 MB) to /system/xbin/hwmirror, preserving each
#     file's own SELinux label, adds the shim as hwcomposer.mtk_common.so and
#     bind-mounts the copy over /vendor/lib64/hw;
#   * restarts the composer, then zygote (the Java side cannot survive a stale
#     SurfaceControl state, and adbd is not touched);
#   * arms device/watchdog.sh, which reboots the phone if the framework is not
#     stable 10 s after the restart.
#
# Everything is runtime-only: `adb reboot` (or -Revert) restores the vendor
# composer. Per-file labels are handled by device/apply.sh -- do not relabel the
# whole directory, it breaks gralloc/vulkan (SF aborts with "no suitable EGLConfig").
param(
    [string]$Serial = $env:ADB_SERIAL,
    [ValidateSet('variant', 'replace', 'none')][string]$Layout = 'variant',
    [int]$Nofence = 1,
    [int]$Rect = 0,
    [int]$Mode = -1,
    [int]$Log = 0,
    [int]$LogEvery = 60,
    [int]$Watchdog = 90,
    [switch]$Disable,
    # fast path: the shim owns the commit and the vendor worker is throttled
    [int]$Fast = 0,
    [int]$Inplace = 0,
    [int]$FastDiff = 0,
    [int]$FastHz = 0,
    [int]$VendHz = 0,
    [int]$UseCt = 0,
    [int]$CtFormat = -1,
    [int]$Dither = -1,
    [int]$Skipconv = 0,
    [int]$SkipconvFmt = 1,
    [int]$Fastgray = 0,
    [int]$Fastconv = 0,
    [int]$Verify = 0,
    [int]$ConvPitch = -1,
    [int]$Skipcopy = 0,
    [int]$CtSrc = 0,
    [int]$Ownbuf = 0,
    [int]$FastIdle = 1,
    [int]$Novsync = 0,
    [int]$EinkVsync = 1,
    [switch]$Revert,
    [switch]$NoRestart
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$shim = Join-Path $here 'out\hwcomposer.mt6765.so'
$real = Join-Path $here 'real\hwcomposer.mt6765.so'
$D = '/data/local/tmp/hwc-shim-mt6765'
$SVC = 'vendor.hwcomposer-2-1'

$adbArgs = @()
if ($Serial) { $adbArgs += @('-s', $Serial) }
function Sh([string]$c) { return ((((& adb @adbArgs shell $c)) 2>&1) | Out-String) }

$id = (Sh 'id -u').Trim()
if ($id -ne '0') {
    (& adb @adbArgs root) 2>&1 | Out-Null
    Start-Sleep -Seconds 3
    if (((Sh 'id -u').Trim()) -ne '0') { throw "not root; run 'adb root', reconnect (persist.adb.tcp.port=5555), retry" }
}
$fp = (Sh 'getprop ro.build.fingerprint').Trim()
if ($fp -notmatch 'treble_arm64') { throw "not the TrebleDroid GSI (fingerprint=$fp)" }
Write-Host "device : $fp"

if ($Revert) {
    Write-Host "reverting: detaching the overlay"
    Sh "umount /vendor/lib64/hw 2>/dev/null; umount -l /vendor/lib64/hw 2>/dev/null; rm -rf /system/xbin/hwmirror; pkill -f 'hwc-shim-mt6765/watchdog.sh'" | Out-Null
    if (-not $NoRestart) {
        Sh "setprop ctl.restart $SVC; sleep 3; setprop ctl.restart zygote" | Out-Null
        Start-Sleep -Seconds 20
    }
    Write-Host ("mount now: " + ((Sh 'mount | grep /vendor/lib64/hw') -replace '\s+$', '') )
    return
}

if (-not (Test-Path $shim)) { throw "build first: pwsh -File build.ps1" }
if (Test-Path $real) {
    $local = (Get-FileHash $real -Algorithm SHA256).Hash.ToLower()
    $dev = ((Sh 'sha256sum /vendor/lib64/hw/hwcomposer.mt6765.so') -split '\s+')[0]
    if ($dev -and $dev -ne $local) { Write-Warning "real/ differs from the device; re-pull /vendor/lib64/hw/hwcomposer.mt6765.so" }
}

$knobs = [ordered]@{
    'vendor.debug.hwc.shim.disable'   = $(if ($Disable) { 1 } else { 0 })
    'vendor.debug.hwc.shim.log'       = $Log
    'vendor.debug.hwc.shim.log_every' = $LogEvery
    'vendor.debug.hwc.shim.nofence'   = $Nofence
    'vendor.debug.hwc.shim.rect'      = $Rect
    'vendor.debug.hwc.shim.mode'      = $Mode
    'vendor.debug.hwc.shim.fast'      = $Fast
    'vendor.debug.hwc.shim.inplace'   = $Inplace
    'vendor.debug.hwc.shim.fast_diff' = $FastDiff
    'vendor.debug.hwc.shim.fast_hz'   = $FastHz
    'vendor.debug.hwc.shim.vend_hz'   = $VendHz
    'vendor.debug.hwc.shim.use_ct'    = $UseCt
    'vendor.debug.hwc.shim.ct_format' = $CtFormat
    'vendor.debug.hwc.shim.dither'    = $Dither
    'vendor.debug.hwc.shim.skipconv'  = $Skipconv
    'vendor.debug.hwc.shim.skipconv_fmt' = $SkipconvFmt
    'vendor.debug.hwc.shim.fastgray'  = $Fastgray
    'vendor.debug.hwc.shim.fastconv'  = $Fastconv
    'vendor.debug.hwc.shim.verify'    = $Verify
    'vendor.debug.hwc.shim.conv_dst_pitch' = $ConvPitch
    'vendor.debug.hwc.shim.skipcopy'  = $Skipcopy
    'vendor.debug.hwc.shim.ctsrc'     = $CtSrc
    'vendor.debug.hwc.shim.ownbuf'    = $Ownbuf
    'vendor.debug.hwc.shim.fast_idle' = $FastIdle
    'vendor.debug.hwc.shim.novsync'   = $Novsync
    # Stock e-ink stack defaults (HiBreak Pro LineageOS device tree, system.prop +
    # init.hibreakpro.eink.rc). The stock framework sets these; a GSI does not, and the
    # vendor composer's e-ink policy expects them.
    'vendor.xrz.disable_eink_vsync'   = $EinkVsync
    'vendor.xrz.hwc_idle'             = 1
    'vendor.xrz.auto_clean_enabled'   = 1
    'vendor.xrz.anti_flicker_enabled' = 1
}
foreach ($k in $knobs.Keys) { Sh "setprop $k $($knobs[$k])" | Out-Null }
Write-Host ("knobs  : " + (($knobs.GetEnumerator() | ForEach-Object { "$($_.Key.Replace('vendor.debug.hwc.shim.',''))=$($_.Value)" }) -join ' '))

Sh "mkdir -p $D" | Out-Null
(& adb @adbArgs push $shim "$D/hwcshim.mt6765.so") 2>&1 | Out-Null
(& adb @adbArgs push (Join-Path $here 'device\apply.sh') "$D/apply.sh") 2>&1 | Out-Null
(& adb @adbArgs push (Join-Path $here 'device\watchdog.sh') "$D/watchdog.sh") 2>&1 | Out-Null
Sh "chmod 644 $D/hwcshim.mt6765.so; chmod 755 $D/apply.sh $D/watchdog.sh" | Out-Null

Write-Host "installing layout '$Layout' and restarting the display stack"
if ($NoRestart) {
    Sh "sh $D/apply.sh $Layout $Watchdog" | Write-Host   # apply.sh still restarts; see note below
} else {
    Sh "sh $D/apply.sh $Layout $Watchdog" | Write-Host
}

Write-Host ""
Write-Host ("composer: " + ((Sh 'pidof android.hardware.graphics.composer@2.1-service')).Trim())
Write-Host "logs   : adb shell 'logcat -d -s hwcshim'"
Write-Host "revert : pwsh -File deploy.ps1 -Revert -Serial $Serial   (or: adb reboot)"
