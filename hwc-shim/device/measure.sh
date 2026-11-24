#!/system/bin/sh
# measure.sh [seconds] -- commit rate for the e-ink shim, run entirely on device.
#
# Keeps the panel awake (fake charger + stayon), restarts the bench, clears the
# log, waits N seconds and reports commits/s. Short enough to run over a flaky
# wireless adb link with one round trip:
#
#   adb shell sh /data/local/tmp/measure.sh 5
#
N=${1:-5}
pkg=com.hibreak.einkbench

dumpsys battery set ac 1 >/dev/null 2>&1
dumpsys battery set status 2 >/dev/null 2>&1
settings put global stay_on_while_plugged_in 7 >/dev/null 2>&1
settings put system screen_off_timeout 2147483647 >/dev/null 2>&1
svc power stayon true >/dev/null 2>&1
cmd power wakeup >/dev/null 2>&1
input keyevent 224 >/dev/null 2>&1
input keyevent 82 >/dev/null 2>&1
am force-stop $pkg >/dev/null 2>&1
am start -n $pkg/.AnimActivity >/dev/null 2>&1
setprop vendor.debug.hwc.shim.log_every 1 >/dev/null 2>&1
sleep 2

# Wake only: never toggle the power key here, or a second press can leave the
# panel asleep (DOZE) after the measurement.
input keyevent 224 >/dev/null 2>&1
cmd power wakeup >/dev/null 2>&1
input keyevent 82 >/dev/null 2>&1
sleep 1

BEFORE=$(dumpsys power | grep mWakefulness= | head -1)
FOCUS=$(dumpsys window | grep mCurrentFocus | head -1)
F1=$(dumpsys gfxinfo $pkg | grep -i "Total frames" | head -1)
logcat -c
sleep $N
AFTER=$(dumpsys power | grep mWakefulness= | head -1)
F2=$(dumpsys gfxinfo $pkg | grep -i "Total frames" | head -1)

C=$(logcat -d -s hwcshim | grep -c "commit #")
F=$(logcat -d -s hwcshim | grep -c "fast #")
LAST=$(logcat -d -s hwcshim | grep "fast #" | tail -1)
MODE=$(getprop vendor.debug.hwc.shim.mode)
FAST=$(getprop vendor.debug.hwc.shim.fastconv)
COPY=$(getprop vendor.debug.hwc.shim.skipcopy)
SYNC=$(getprop vendor.debug.hwc.shim.novsync)

echo "mode=$MODE fastconv=$FAST skipcopy=$COPY novsync=$SYNC"
echo "power : $(logcat -d -s hwcshim | grep setPowerMode | tail -1)"
echo "before: $BEFORE"
echo "focus : $FOCUS"
echo "after : $AFTER"
echo "seconds=$N vendor=$C fast=$F"
echo "$LAST"
echo "frames1: $F1"
echo "frames2: $F2"
