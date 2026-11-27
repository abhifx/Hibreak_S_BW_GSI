#!/system/bin/sh
# DEPRECATED/invalid for physical-panel measurement: this drives the old direct
# -Fast 1 -Xproc 1 helper-buffer commit mode, which can show fast software calls
# while the physical panel remains frozen. Use measure-xproc-vendor.sh instead.
# Physical commits are hwcshim "fast #" lines, not SurfaceFlinger/vendor messages.
N=${1:-10}
MODE=${2:-183}
PKG=com.hibreak.einkbench

setprop vendor.debug.hwc.shim.fast_idle 0
setprop vendor.debug.hwc.shim.mode "$MODE"
setprop vendor.xrz.global_refresh_mode "$MODE"
dumpsys battery set ac 1 >/dev/null 2>&1
settings put global stay_on_while_plugged_in 7 >/dev/null 2>&1
svc power stayon true >/dev/null 2>&1
cmd power wakeup >/dev/null 2>&1
input keyevent 224 >/dev/null 2>&1
input keyevent 82 >/dev/null 2>&1
am force-stop "$PKG" >/dev/null 2>&1
am start -n "$PKG/.AnimActivity" >/dev/null 2>&1
sleep 6

logcat -c
sleep "$N"
COMMITS=$(logcat -d -s hwcshim | grep -c 'fast #')
FAILS=$(logcat -d -s hwcshim | grep -c 'xproc convert failed')
VENDOR=$(logcat -d -s hwcshim | grep -c ' pass mode=')
APP=$(logcat -d -s EINKBENCH | tail -5)
LAST=$(logcat -d -s hwcshim | grep 'fast #' | tail -1)
WINDOW=$(logcat -d -s hwcshim | grep 'present window' | tail -3)

echo "seconds=$N mode=$MODE physical_commits=$COMMITS commit_fps=$((COMMITS / N)) xproc_failures=$FAILS vendor_commits=$VENDOR"
echo "app_choreographer_fps:"
echo "$APP"
echo "last_fast=$LAST"
echo "present_windows:"
echo "$WINDOW"
