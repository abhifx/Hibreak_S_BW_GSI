#!/bin/sh
# deploy-pro-sf.sh -- try the HiBreak Pro's patched SurfaceFlinger on the GSI.
#
#   adb root; then:
#     ./tools/deploy-pro-sf.sh [pro-sf-dir] [serial]
#     ./tools/deploy-pro-sf.sh -Revert [serial]
#
# What it does (all reversible by reboot):
#   * pushes pro-sf/surfaceflinger (and any lib64/*.so) to /data/local/tmp/sfpro
#   * copies them to /system/xbin/sfpro (tmpfs, system_file label) and bind-mounts
#     the binary over /system/bin/surfaceflinger, and each lib over its GSI path
#   * arms the HWC watchdog, restarts surfaceflinger, then zygote
#
# If the UI does not come back: `adb reboot`, or run with -Revert while adb still
# works from the same boot.
set -eu

if [ "${1:-}" = "-Revert" ]; then
    SERIAL=${2:-}
    A="adb ${SERIAL:+-s $SERIAL}"
    echo "==> reverting surfaceflinger overlay"
    $A shell 'umount /system/bin/surfaceflinger 2>/dev/null;
              for l in /system/xbin/sfpro/lib64/*; do
                  n=$(basename $l); umount /system/lib64/$n 2>/dev/null; done;
              mount | grep sfpro | head' || true
    $A shell 'setprop ctl.restart surfaceflinger; sleep 3; setprop ctl.restart zygote'
    echo "reverted"
    exit 0
fi

DIR=${1:-pro-sf}
SERIAL=${2:-}
A="adb ${SERIAL:+-s $SERIAL}"

[ -s "$DIR/surfaceflinger" ] || { echo "missing $DIR/surfaceflinger (run extract-pro-sf.sh)" >&2; exit 1; }

echo "==> device"
FP=$($A shell getprop ro.build.fingerprint | tr -d '\r')
echo "    $FP"
case "$FP" in *treble*) ;; *) echo "not the TD GSI" >&2; exit 1 ;; esac
[ "$($A shell id -u | tr -d '\r')" = "0" ] || { echo "need adb root" >&2; exit 1; }

echo "==> push"
$A shell 'rm -rf /system/xbin/sfpro; mkdir -p /system/xbin/sfpro/lib64'
$A push "$DIR/surfaceflinger" /data/local/tmp/sfpro.bin >/dev/null
$A shell 'cp /data/local/tmp/sfpro.bin /system/xbin/sfpro/surfaceflinger; chmod 755 /system/xbin/sfpro/surfaceflinger; chcon u:object_r:system_file:s0 /system/xbin/sfpro/surfaceflinger'
if [ -d "$DIR/lib64" ]; then
    for f in "$DIR"/lib64/*.so; do
        [ -e "$f" ] || continue
        n=$(basename "$f")
        echo "    lib $n"
        $A push "$f" "/data/local/tmp/$n" >/dev/null
        $A shell "cp /data/local/tmp/$n /system/xbin/sfpro/lib64/$n; chmod 644 /system/xbin/sfpro/lib64/$n; chcon u:object_r:system_file:s0 /system/xbin/sfpro/lib64/$n"
    done
fi

echo "==> bind mounts"
$A shell 'mount --bind /system/xbin/sfpro/surfaceflinger /system/bin/surfaceflinger && echo "  sf mounted"
          for l in /system/xbin/sfpro/lib64/*; do
              [ -e "$l" ] || continue; n=$(basename "$l")
              mount --bind "$l" /system/lib64/$n && echo "  lib $n mounted"
          done'

echo "==> watchdog + restart"
$A push build/hwc-shim-mt6765/device/watchdog.sh /data/local/tmp/sfpro-watchdog.sh >/dev/null 2>&1 || true
$A shell 'chmod 755 /data/local/tmp/sfpro-watchdog.sh 2>/dev/null; nohup sh /data/local/tmp/sfpro-watchdog.sh 120 >/dev/null 2>&1 &'
$A shell 'setprop ctl.restart surfaceflinger'
sleep 4
$A shell 'setprop ctl.restart zygote'
sleep 8
echo "==> state"
$A shell 'getprop init.svc.surfaceflinger; pidof surfaceflinger; pidof system_server'
echo
echo "If the UI is alive, check: adb shell dumpsys SurfaceFlinger | head -30"
echo "Revert: $0 -Revert $SERIAL   (or: adb reboot)"
