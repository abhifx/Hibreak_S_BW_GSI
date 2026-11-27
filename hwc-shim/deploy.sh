#!/usr/bin/env bash
# Deploy the mt6765 HWC shim on the TD A16 GSI from a Linux host, reversibly.
#
# deploy.ps1 does the same on Windows and has the full knob list; this is the
# Linux path for a session that has no pwsh. Same guarantees: the shim is a bind
# mount over /vendor/lib64/hw, per-file SELinux labels are preserved by
# device/apply.sh, and E-ink Center reapplies it after boot. --revert writes
# persist.disabled so reboots stay on the vendor composer.
#
#   ./deploy.sh                              # install with the shim's defaults
#   ./deploy.sh ctprobe=1 log=1 log_every=30 # any vendor.debug.hwc.shim.<name>=<v>
#   ./deploy.sh --knob fastconv=1 --knob rect=1
#   ./deploy.sh --revert
#
# Knobs given without --knob are still accepted for convenience.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
D=/data/local/tmp/hwc-shim-mt6765
SVC=vendor.hwcomposer-2-1
EXPECTED_VENDOR_HASH=51827cc37c89dc4aef99b0da39f5200e13fa92014c1b7d04c6b41ff4c26200c6
watchdog=90
revert=0

knobs=()
while [ $# -gt 0 ]; do
  case "$1" in
    --revert) revert=1; shift ;;
    --knob) knobs+=("$2"); shift 2 ;;
    --watchdog) watchdog=$2; shift 2 ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *) knobs+=("$1"); shift ;;
  esac
done
case "$watchdog" in
  ''|*[!0-9]*) echo "watchdog delay must be an integer" >&2; exit 2 ;;
esac
[ "$watchdog" -ge 5 ] && [ "$watchdog" -le 600 ] || {
  echo "watchdog delay must be 5..600 seconds" >&2; exit 2; }

adb wait-for-device
if [ "$(adb shell id -u | tr -d '\r')" != "0" ]; then
  adb root >/dev/null 2>&1 || true
  adb wait-for-device
  [ "$(adb shell id -u | tr -d '\r')" = "0" ] || { echo "not root" >&2; exit 1; }
fi
fp=$(adb shell getprop ro.build.fingerprint | tr -d '\r')
case "$fp" in *treble_arm64*) ;; *) echo "not the TrebleDroid GSI: $fp" >&2; exit 1 ;; esac
platform=$(adb shell getprop ro.board.platform | tr -d '\r')
board=$(adb shell getprop ro.product.board | tr -d '\r')
abi=$(adb shell getprop ro.product.cpu.abi | tr -d '\r')
[ "$platform" = "mt6765" ] || { echo "unexpected platform: $platform" >&2; exit 1; }
case "$board" in mt6765|k65v1_64_bsp) ;; *)
  echo "unexpected board: $board" >&2; exit 1 ;; esac
[ "$abi" = "arm64-v8a" ] || { echo "unexpected ABI: $abi" >&2; exit 1; }
echo "device : $fp ($board/$abi)"

if [ "$revert" = 1 ]; then
  echo "reverting: detaching the overlay and disabling boot persistence"
  adb shell "touch $D/persist.disabled; rm -f $D/persist.boot" || true
  adb shell "umount /vendor/lib64/hw 2>/dev/null; umount -l /vendor/lib64/hw 2>/dev/null" || true
  if adb shell 'grep " /vendor/lib64/hw " /proc/mounts' | grep -q .; then
    echo "vendor HWC bind mount is still active; reboot to restore the vendor composer" >&2
    exit 1
  fi
  adb shell 'if [ -f '$D'/props-before.env ]; then while IFS="=" read -r k v; do setprop "$k" "$v"; done < '$D'/props-before.env; fi' || true
  adb shell 'if [ -f '$D'/eink-debug-before.env ]; then while IFS="=" read -r f v; do echo "$v" > /sys/kernel/debug/eink_debug/$f; done < '$D'/eink-debug-before.env; fi' || true
  adb shell "setprop vendor.debug.hwc.shim.helper_recovery 0" || true
  adb shell "rm -f $D/props-before.env $D/eink-debug-before.env" || true
  adb shell "rm -rf /system/xbin/hwmirror; pkill -f '[h]wc-shim-mt6765/watchdog.sh'; pkill -f '[h]wc-shim-mt6765/helper-watchdog.sh'; pkill gray_helper 2>/dev/null; pkill -9 gray_helper 2>/dev/null; true" || true
  adb shell "setprop ctl.restart $SVC; sleep 3; setprop ctl.restart zygote" || true
  sleep 20
  adb shell 'mount | grep /vendor/lib64/hw' || echo "overlay detached"
  exit 0
fi

shim="$here/out/hwcomposer.mt6765.so"
[ -f "$shim" ] || { echo "build first" >&2; exit 1; }
[ -f "$here/real/hwcomposer.mt6765.so" ] || {
  echo "missing real/hwcomposer.mt6765.so; cannot verify vendor ABI" >&2; exit 1; }
l=$(sha256sum "$here/real/hwcomposer.mt6765.so" | cut -d' ' -f1)
[ "$l" = "$EXPECTED_VENDOR_HASH" ] || {
  echo "local vendor composer hash $l is not the approved $EXPECTED_VENDOR_HASH" >&2; exit 1; }
d=$(adb shell sha256sum /vendor/lib64/hw/hwcomposer.mt6765.so | tr -d '\r' | cut -d' ' -f1)
if [ "$d" != "$EXPECTED_VENDOR_HASH" ]; then
  d=$(adb shell "sha256sum /vendor/lib64/hw/hwcomposer.mt6765.real.so 2>/dev/null || true" | tr -d '\r' | cut -d' ' -f1)
fi
[ "$d" = "$EXPECTED_VENDOR_HASH" ] || {
  echo "device vendor composer hash $d is not the approved $EXPECTED_VENDOR_HASH" >&2; exit 1; }
echo "vendor : $EXPECTED_VENDOR_HASH (verified)"

# Stock e-ink stack defaults the GSI lacks (HiBreak Pro device tree, patch notes).
base=(log=1 log_every=60 fast=0 fastconv=0 rect=0 nofence=0 disable=0
      mode=-1 xproc=1 xproc_vendor=1 xproc_damage=0 helper_onebit=0 skipcopy=0 fast_idle=1)
knob_value() {
  key=$1
  def=$2
  val=$def
  for kv in "${base[@]}" "${knobs[@]}"; do
    case "$kv" in "$key="*) val=${kv#*=} ;; esac
  done
  printf '%s' "$val"
}
fast=$(knob_value fast 0)
xproc=$(knob_value xproc 0)
xproc_vendor=$(knob_value xproc_vendor 0)
skipcopy=$(knob_value skipcopy 0)
mode=$(knob_value mode -1)
disable=$(knob_value disable 0)
[ "$skipcopy" = 0 ] || { echo "unsafe: skipcopy must stay 0" >&2; exit 2; }
if [ "$fast" = 1 ] && { [ "$xproc" = 1 ] || [ "$xproc_vendor" = 1 ]; }; then
  echo "unsafe: Fast=1 with xproc is the rejected direct-commit path" >&2; exit 2
fi
if [ "$xproc_vendor" = 1 ] && [ "$xproc" != 1 ]; then
  echo "xproc_vendor requires xproc=1" >&2; exit 2
fi
if [ "$xproc" = 1 ] && [ "$xproc_vendor" != 1 ]; then
  echo "xproc=1 is supported only with xproc_vendor=1" >&2; exit 2
fi
if [ "$disable" = 1 ] && { [ "$xproc" = 1 ] || [ "$xproc_vendor" = 1 ]; }; then
  echo "disable cannot be combined with helper startup" >&2; exit 2
fi
if [ "$xproc_vendor" = 1 ]; then
  case "$mode" in -1|16|178|179|183) ;; *)
    echo "unsafe mode $mode: helper supports A2/Fast/Extreme/Hyper only" >&2; exit 2 ;; esac
fi

adb shell mkdir -p $D
declare -A seen_props
prop_keys=()
for kv in "${base[@]}" "${knobs[@]}"; do
  key=${kv%%=*}
  if [ -z "${seen_props[$key]:-}" ]; then
    seen_props[$key]=1
    prop_keys+=("$key")
  fi
done
snapshot=$(adb shell "test -f $D/props-before.env; echo \$?" | tr -d '\r')
if [ "$snapshot" != 0 ]; then
  adb shell ": > $D/props-before.env"
  for key in "${prop_keys[@]}"; do
    value=$(adb shell "getprop vendor.debug.hwc.shim.$key" | tr -d '\r')
    adb shell "printf '%s=%s\n' 'vendor.debug.hwc.shim.$key' '$value' >> $D/props-before.env"
  done
  for key in vendor.xrz.disable_eink_vsync vendor.xrz.hwc_idle \
             vendor.xrz.auto_clean_enabled vendor.xrz.anti_flicker_enabled; do
    value=$(adb shell "getprop $key" | tr -d '\r')
    adb shell "printf '%s=%s\n' '$key' '$value' >> $D/props-before.env"
  done
fi

set_prop() { adb shell setprop "vendor.debug.hwc.shim.$1" "$2"; }
for kv in "${base[@]}" "${knobs[@]}"; do
  set_prop "${kv%%=*}" "${kv#*=}"
done
adb shell setprop vendor.xrz.disable_eink_vsync 1
adb shell setprop vendor.xrz.hwc_idle 1
adb shell setprop vendor.xrz.auto_clean_enabled 1
adb shell setprop vendor.xrz.anti_flicker_enabled 1
adb shell setprop debug.sf.luma_sampling 0
echo "knobs  : ${base[*]} ${knobs[*]:-}"

adb push "$shim" "$D/hwcshim.mt6765.so" >/dev/null
adb push "$here/device/apply.sh" "$D/apply.sh" >/dev/null
adb push "$here/device/watchdog.sh" "$D/watchdog.sh" >/dev/null
adb push "$here/device/helper-watchdog.sh" "$D/helper-watchdog.sh" >/dev/null
adb push "$here/device/persist.sh" "$D/persist.sh" >/dev/null
adb shell "chmod 644 $D/hwcshim.mt6765.so; chmod 755 $D/apply.sh $D/watchdog.sh $D/helper-watchdog.sh $D/persist.sh"

adb shell ": > $D/persist.env"
for key in "${prop_keys[@]}"; do
  value=$(adb shell "getprop vendor.debug.hwc.shim.$key" | tr -d '\r')
  adb shell "printf '%s=%s\n' 'vendor.debug.hwc.shim.$key' '$value' >> $D/persist.env"
done
for key in vendor.xrz.disable_eink_vsync vendor.xrz.hwc_idle \
           vendor.xrz.auto_clean_enabled vendor.xrz.anti_flicker_enabled; do
  value=$(adb shell "getprop $key" | tr -d '\r')
  adb shell "printf '%s=%s\n' '$key' '$value' >> $D/persist.env"
done
adb shell "printf '%s\n' 'debug.sf.luma_sampling=0' >> $D/persist.env"
adb shell "printf '%s\n' variant > $D/persist.layout; printf '%s\n' '$watchdog' > $D/persist.watchdog; rm -f $D/persist.disabled $D/persist.boot"

adb shell "pkill -f '[h]wc-shim-mt6765/helper-watchdog.sh' 2>/dev/null; pkill gray_helper 2>/dev/null; pkill -9 gray_helper 2>/dev/null; true"
if [ "$xproc" = 1 ]; then
  helper="$here/gray-helper/out/gray_helper"
  [ -f "$helper" ] || { echo "xproc requested but $helper is missing" >&2; exit 1; }
  adb push "$helper" "$D/gray_helper" >/dev/null
  adb shell "chmod 755 $D/gray_helper; nohup $D/gray_helper --connect hwcgray_hwc >$D/gray-helper.log 2>&1 </dev/null &"
  sleep 1
  pid=$(adb shell "pidof gray_helper || true" | tr -d '\r')
  [ -n "$pid" ] || { echo "gray_helper failed to start; inspect $D/gray-helper.log" >&2; exit 1; }
  adb shell "setprop vendor.debug.hwc.shim.helper_recovery 0"
  adb shell "nohup sh $D/helper-watchdog.sh >/dev/null 2>&1 </dev/null &"
  echo "helper : pid=$pid abstract=hwcgray_hwc"
else
  echo "helper : stopped (xproc=0)"
fi

echo "installing the overlay and restarting the display stack (screen will blank)"
adb shell "sh $D/apply.sh variant $watchdog"
if ! adb shell 'grep -q " /vendor/lib64/hw " /proc/mounts'; then
  echo "shim bind mount is not active" >&2; exit 1
fi
sleep 2
echo "composer: $(adb shell pidof android.hardware.graphics.composer@2.1-service | tr -d '\r')"
echo "persist: E-ink Center will reapply this profile after DSU reboot"
