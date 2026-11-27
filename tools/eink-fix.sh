#!/usr/bin/env bash
# Diagnose / fix the e-ink panel on the Android 16 DSU (HiBreak S).
# Run on Arch with the phone connected and USB debugging available.
set -uo pipefail

ADB="${ADB:-adb}"

echo "==> Waiting for device (plug USB, power on)..."
$ADB wait-for-device || { echo "No adb device. Check USB/power."; exit 1; }

echo
echo "==> Build:"
$ADB shell getprop ro.build.fingerprint
$ADB shell getprop ro.build.version.release
$ADB shell getprop ro.boot.flash.locked
$ADB shell getprop ro.boot.verifiedbootstate

echo
echo "==> Trying adb root (TrebleDroid is userdebug):"
$ADB root 2>&1 || true
sleep 2
$ADB wait-for-device || true
$ADB shell id

echo
echo "==> Pro GSI display fix: disable HW overlays"
$ADB shell service call SurfaceFlinger 1008 i32 1 2>&1 || true
sleep 1
$ADB shell wm size
$ADB shell wm density

echo
echo "==> E Ink debug interface:"
$ADB shell "ls -l /sys/kernel/debug/eink_debug 2>&1"
for f in global_mode mode refresh_mode waveform machine_waveform vcom; do
  $ADB shell "cat /sys/kernel/debug/eink_debug/$f 2>/dev/null" | sed "s/^/  $f: /"
done

echo
echo "==> Kernel e-ink messages:"
$ADB shell "dmesg 2>/dev/null | grep -iE 'eink|papyrus|syv|tps65185|lm3630|epd' | tail -40"

echo
echo "==> logcat display errors:"
$ADB shell "logcat -d 2>/dev/null | grep -iE 'eink|surfaceflinger|hwc|display' | tail -40"

echo
echo "==> Vendor EPD packages present?"
$ADB shell "pm list packages | grep -iE 'xrz|eink|epd' || echo none"

echo
echo "==> If the screen is still frozen, try setting the stock default mode:"
echo "    $ADB shell 'echo 178 > /sys/kernel/debug/eink_debug/global_mode'"
echo "    (run as root; 178 is ro.vendor.xrz.default_refresh_mode on stock)"
