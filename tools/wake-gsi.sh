#!/usr/bin/env bash
# Wake and keep the e-ink display on under the Android 16 GSI.
# The GSI's power manager puts the panel into DOZE/OFF after boot; the e-ink
# panel then holds the last image (looks like a frozen Bigme logo).
set -uo pipefail
ADB="${ADB:-adb}"

"$ADB" wait-for-device || { echo "no adb device"; exit 1; }

"$ADB" shell cmd power wakeup 2>/dev/null || true
"$ADB" shell input keyevent 224 2>/dev/null || true   # WAKEUP
"$ADB" shell input keyevent 82 2>/dev/null || true    # MENU/unlock
"$ADB" shell svc power stayon true 2>/dev/null || true
"$ADB" shell settings put system screen_off_timeout 2147483647 2>/dev/null || true
"$ADB" shell settings put global stay_on_while_plugged_in 7 2>/dev/null || true
"$ADB" shell settings put system screen_brightness 128 2>/dev/null || true
"$ADB" shell dumpsys deviceidle disable 2>/dev/null || true

sleep 1
echo "==> Display:"
"$ADB" shell dumpsys display 2>/dev/null | grep -E 'Display State|mState=|mScreenState=' | head -5
echo "==> E Ink mode/enable:"
"$ADB" shell "cat /sys/kernel/debug/eink_debug/global_mode; cat /sys/kernel/debug/eink_debug/enable" 2>/dev/null
