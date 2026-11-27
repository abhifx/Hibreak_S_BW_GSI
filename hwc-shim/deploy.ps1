# Deploy the mt6765 HWC shim on the TD A16 GSI, reversibly.
#
#   pwsh -File deploy.ps1 -Serial 192.168.29.149:5555            # install + restart
#   pwsh -File deploy.ps1 -Serial ... -Nofence 0                 # A/B the fence change
#   pwsh -File deploy.ps1 -Serial ... -Revert                    # umount, back to vendor
#
# What it does (all verified on device 2026-11-20):
#   * copies /vendor/lib64/hw (7.5 MB) to /system/xbin/hwmirror, preserving each
#     file's own SELinux label, adds the shim as hwcomposer.mtk_common.so and
#     bind-mounts the copy over /vendor/lib64/hw;
#   * restarts the composer, then zygote (the Java side cannot survive a stale
#     SurfaceControl state, and adbd is not touched);
#   * with -Xproc 1, starts gray_helper in abstract-socket client mode; HWC owns
#     the listener, so SELinux stays enforcing and no filesystem socket is used;
#     add -XprocVendor 1 for the visible-60-fps vendor-pipeline configuration;
#   * arms device/watchdog.sh, which reboots the phone if the framework is not
#     stable 10 s after the restart.
#
# Everything is runtime-only. With persistence enabled, E-ink Center reapplies
# the overlay after a DSU reboot. `deploy.ps1 -Revert` writes persist.disabled so
# reboots stay on the vendor composer. Per-file labels are handled by
# device/apply.sh -- do not relabel the whole directory, it breaks gralloc/vulkan
# (SF aborts with "no suitable EGLConfig").
param(
    [string]$Serial = $env:ADB_SERIAL,
    [ValidateSet('variant', 'replace', 'none')][string]$Layout = 'variant',
    [int]$Nofence = 0,
    [int]$Rect = 0,
    [int]$Mode = -1,
    [int]$Log = 1,
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
    [int]$DamageCopy = 0,
    [int]$Xproc = 1,
    [int]$XprocVendor = 1,
    [int]$XprocDamage = 0,
    [int]$HelperOnebit = 0,
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
$helper = Join-Path $here 'gray-helper\out\gray_helper'
$real = Join-Path $here 'real\hwcomposer.mt6765.so'
$D = '/data/local/tmp/hwc-shim-mt6765'
$SVC = 'vendor.hwcomposer-2-1'
$ExpectedVendorHash = '51827cc37c89dc4aef99b0da39f5200e13fa92014c1b7d04c6b41ff4c26200c6'
$ExpectedPlatform = 'mt6765'
$ExpectedBoards = @('mt6765', 'k65v1_64_bsp')

$adbArgs = @()
if ($Serial) { $adbArgs += @('-s', $Serial) }
function Sh([string]$c) { return ((((& adb @adbArgs shell $c)) 2>&1) | Out-String) }
function ShChecked([string]$c) {
    $out = (((& adb @adbArgs shell $c) 2>&1) | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "adb shell failed ($LASTEXITCODE): $c`n$out" }
    return $out
}
function Push([string]$src, [string]$dst) {
    (& adb @adbArgs push $src $dst) 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "adb push failed: $src -> $dst" }
}

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
    Write-Host "reverting: detaching the overlay and disabling boot persistence"
    Sh "touch $D/persist.disabled; rm -f $D/persist.boot" | Out-Null
    Sh "umount /vendor/lib64/hw 2>/dev/null; umount -l /vendor/lib64/hw 2>/dev/null" | Out-Null
    if ((Sh 'grep " /vendor/lib64/hw " /proc/mounts || true').Trim()) {
        throw "The vendor HWC bind mount is still active; refusing to delete its backing mirror. Reboot to restore the vendor composer."
    }
    Sh ('if [ -f ' + $D + '/props-before.env ]; then while IFS="=" read -r k v; do setprop "$k" "$v"; done < ' + $D + '/props-before.env; fi') | Out-Null
    Sh ('if [ -f ' + $D + '/eink-debug-before.env ]; then while IFS="=" read -r f v; do echo "$v" > /sys/kernel/debug/eink_debug/$f; done < ' + $D + '/eink-debug-before.env; fi') | Out-Null
    Sh "setprop vendor.debug.hwc.shim.helper_recovery 0" | Out-Null
    Sh "rm -f $D/props-before.env $D/eink-debug-before.env" | Out-Null
    Sh "rm -rf /system/xbin/hwmirror; pkill -f '[h]wc-shim-mt6765/watchdog.sh'; pkill -f '[h]wc-shim-mt6765/helper-watchdog.sh'; pkill gray_helper 2>/dev/null; pkill -9 gray_helper 2>/dev/null; rm -f /data/local/tmp/hwcgray.sock /data/vendor/hwcgray.sock" | Out-Null
    if (-not $NoRestart) {
        Sh "setprop ctl.restart $SVC; sleep 3; setprop ctl.restart zygote" | Out-Null
        Start-Sleep -Seconds 20
    }
    Write-Host ("mount now: " + ((Sh 'mount | grep /vendor/lib64/hw') -replace '\s+$', '') )
    return
}

if ($NoRestart) {
    throw "-NoRestart is not supported: apply.sh must restart the composer and zygote to leave a coherent display stack"
}
if ($Fast -eq 1 -and ($Xproc -eq 1 -or $XprocVendor -eq 1)) {
    throw "unsafe combination: direct Fast=1 with Xproc/XprocVendor is the rejected helper-buffer commit path"
}
if ($Skipcopy -ne 0) {
    throw "unsafe option: Skipcopy must stay 0; without the matching helper copy it can commit black frames"
}
if ($XprocVendor -eq 1 -and $Xproc -ne 1) { throw "-XprocVendor requires -Xproc 1" }
if ($Disable -and ($Xproc -eq 1 -or $XprocVendor -eq 1)) {
    throw "unsafe combination: -Disable cannot be combined with helper startup"
}
if ($XprocVendor -eq 1 -and $Mode -ne -1 -and $Mode -notin @(16, 178, 179, 183)) {
    throw "unsafe mode: XprocVendor is validated only for A2/Fast/Extreme/Hyper (16/178/179/183)"
}
if (-not (Test-Path $shim)) { throw "build first: pwsh -File build.ps1" }
if ($Xproc -and -not (Test-Path $helper)) { throw "xproc requested; build gray-helper first: pwsh -File gray-helper/build.ps1" }

$platform = (Sh 'getprop ro.board.platform').Trim()
$board = (Sh 'getprop ro.product.board').Trim()
$abi = (Sh 'getprop ro.product.cpu.abi').Trim()
if ($platform -ne $ExpectedPlatform -or $ExpectedBoards -notcontains $board) {
    throw "unexpected platform board=$board platform=$platform (expected $ExpectedPlatform / $($ExpectedBoards -join ','))"
}
if ($abi -ne 'arm64-v8a') { throw "unexpected ABI '$abi' (expected arm64-v8a)" }
if (-not (Test-Path $real)) {
    throw "missing real/hwcomposer.mt6765.so; cannot verify the vendor ABI"
}
$local = (Get-FileHash $real -Algorithm SHA256).Hash.ToLower()
if ($local -ne $ExpectedVendorHash) {
    throw "real/hwcomposer.mt6765.so hash $local does not match the approved vendor ABI $ExpectedVendorHash"
}
$dev = ((Sh 'sha256sum /vendor/lib64/hw/hwcomposer.mt6765.so') -split '\s+')[0].ToLower()
if ($dev -ne $ExpectedVendorHash) {
    $devReal = ((Sh 'sha256sum /vendor/lib64/hw/hwcomposer.mt6765.real.so 2>/dev/null') -split '\s+')[0].ToLower()
    if ($devReal -eq $ExpectedVendorHash) { $dev = $devReal }
}
if ($dev -ne $ExpectedVendorHash) {
    throw "device vendor composer hash '$dev' does not match the approved ABI $ExpectedVendorHash"
}
Write-Host "vendor : $ExpectedVendorHash (verified)"

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
    'vendor.debug.hwc.shim.damagecopy' = $DamageCopy
    'vendor.debug.hwc.shim.xproc'      = $Xproc
    'vendor.debug.hwc.shim.xproc_vendor' = $XprocVendor
    'vendor.debug.hwc.shim.xproc_damage' = $XprocDamage
    'vendor.debug.hwc.shim.helper_onebit' = $HelperOnebit
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
    # Brightness is manual on this device; skip SF's extra luma/region-sampling
    # composition passes to reduce GPU work and buffer pressure.
    'debug.sf.luma_sampling'          = 0
}

# Preserve the first pre-deployment values so -Revert restores properties too.
$snap = (Sh "test -f $D/props-before.env; echo `$?").Trim()
if ($snap -ne '0') {
    Sh "mkdir -p $D; : > $D/props-before.env" | Out-Null
    foreach ($k in $knobs.Keys) {
        $v = (Sh "getprop $k").Trim()
        Sh "printf '%s=%s\n' '$k' '$v' >> $D/props-before.env" | Out-Null
    }
}

foreach ($k in $knobs.Keys) {
    Sh "setprop $k $($knobs[$k])" | Out-Null
    $actual = (Sh "getprop $k").Trim()
    if ($actual -ne [string]$knobs[$k]) { throw "setprop $k failed: wanted $($knobs[$k]), got '$actual'" }
}
Write-Host ("knobs  : " + (($knobs.GetEnumerator() | ForEach-Object { "$($_.Key.Replace('vendor.debug.hwc.shim.',''))=$($_.Value)" }) -join ' '))

ShChecked "mkdir -p $D" | Out-Null
Push $shim "$D/hwcshim.mt6765.so"
Push (Join-Path $here 'device\apply.sh') "$D/apply.sh"
Push (Join-Path $here 'device\watchdog.sh') "$D/watchdog.sh"
Push (Join-Path $here 'device\helper-watchdog.sh') "$D/helper-watchdog.sh"
Push (Join-Path $here 'device\persist.sh') "$D/persist.sh"
ShChecked "chmod 644 $D/hwcshim.mt6765.so; chmod 755 $D/apply.sh $D/watchdog.sh $D/helper-watchdog.sh $D/persist.sh" | Out-Null

# Save the exact deployed profile for the boot re-apply path. A normal deploy
# re-enables persistence; -Revert leaves persist.disabled behind.
$persistLines = ($knobs.GetEnumerator() | ForEach-Object { "'$($_.Key)=$($_.Value)'" }) -join ' '
ShChecked "printf '%s\n' $persistLines > $D/persist.env; printf '%s\n' '$Layout' > $D/persist.layout; printf '%s\n' '$Watchdog' > $D/persist.watchdog; rm -f $D/persist.disabled $D/persist.boot" | Out-Null

ShChecked "pkill -f '[h]wc-shim-mt6765/helper-watchdog.sh' 2>/dev/null; pkill gray_helper 2>/dev/null; pkill -9 gray_helper 2>/dev/null; rm -f /data/local/tmp/hwcgray.sock /data/vendor/hwcgray.sock; true" | Out-Null
if ($Xproc) {
    Push $helper "$D/gray_helper"
    ShChecked "chmod 755 $D/gray_helper; nohup $D/gray_helper --connect hwcgray_hwc >$D/gray-helper.log 2>&1 </dev/null &" | Out-Null
    Start-Sleep -Milliseconds 700
    $helperPid = (Sh 'pidof gray_helper').Trim()
    if (-not $helperPid) {
        throw "gray helper failed to start; inspect: adb shell cat $D/gray-helper.log"
    }
    ShChecked "setprop vendor.debug.hwc.shim.helper_recovery 0" | Out-Null
    Write-Host "helper  : pid=$helperPid abstract=hwcgray_hwc"
    ShChecked "nohup sh $D/helper-watchdog.sh >/dev/null 2>&1 </dev/null &" | Out-Null
    Write-Host "helper watchdog: started"
} else {
    Write-Host "helper  : stopped (xproc=0)"
}

Write-Host "installing layout '$Layout' and restarting the display stack"
$applyOut = ShChecked "sh $D/apply.sh $Layout $Watchdog"
$applyOut | Write-Host
if ($applyOut -notmatch 'layout=') { throw "apply.sh did not report a completed layout" }
if ($applyOut -notmatch 'watchdog:') { throw "apply.sh did not report watchdog startup" }
if (-not (Sh 'grep " /vendor/lib64/hw " /proc/mounts || true').Trim()) {
    throw "the shim bind mount is not active; run -Revert and inspect apply.sh output"
}
Write-Host ("mount  : " + ((Sh 'grep " /vendor/lib64/hw " /proc/mounts').Trim()))

Write-Host ""
Write-Host ("composer: " + ((Sh 'pidof android.hardware.graphics.composer@2.1-service')).Trim())
Write-Host "logs   : adb shell 'logcat -d -s hwcshim'"
Write-Host "persist: E-ink Center will reapply this profile after DSU reboot"
Write-Host "revert : pwsh -File deploy.ps1 -Revert -Serial $Serial   (disables boot persistence)"
