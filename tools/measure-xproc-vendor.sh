#!/system/bin/sh
# Measure the correctness-first xproc vendor-pipeline mode. Physical commits are
# the vendor's own "commit #" lines, not direct shim commits.
#
# Every property/setting this script changes is restored on exit, including on
# Ctrl-C or a failed adb command. Runs are serialized because they temporarily
# change shared HWC properties. Each run uses a private log file and an exact
# millisecond capture window, so stale log data cannot inflate the count.
N=${1:-10}
MODE=${2:-178}
FORCE=${3:-0}
PKG=com.hibreak.einkbench
LOCK=/data/local/tmp/xproc-measure.lock
TMP=
LOGCAT_PID=
LOCKED=0
RESTORE_READY=0
OLD_ACCESSIBILITY=
OLD_ACCESSIBILITY_ENABLED=
MEASURE_ACCESSIBILITY=

case "$N" in
  ''|*[!0-9]*) echo "N must be 1..120 seconds" >&2; exit 2 ;;
esac
[ "$N" -ge 1 ] && [ "$N" -le 120 ] || { echo "N must be 1..120 seconds" >&2; exit 2; }
case "$FORCE" in 0|1) ;; *) echo "FORCE must be 0 or 1" >&2; exit 2 ;; esac
case "$MODE" in
  16|178|179|183) ;;
  *) echo "MODE must be A2/Fast/Extreme/Hyper (16/178/179/183)" >&2; exit 2 ;;
esac

restore() {
  [ "$RESTORE_READY" = 1 ] || return 0
  setprop vendor.debug.hwc.shim.mode "$OLD_SHIM_MODE"
  setprop vendor.debug.hwc.shim.log "$OLD_SHIM_LOG"
  setprop vendor.debug.hwc.shim.log_every "$OLD_SHIM_LOG_EVERY"
  if [ "$FORCE" = 1 ]; then
    setprop vendor.xrz.global_refresh_mode "$OLD_XRZ_MODE"
  fi
  if [ "$OLD_STAY" = "null" ] || [ -z "$OLD_STAY" ]; then
    settings delete global stay_on_while_plugged_in >/dev/null 2>&1
  else
    settings put global stay_on_while_plugged_in "$OLD_STAY" >/dev/null 2>&1
  fi
  svc power stayon false >/dev/null 2>&1
  dumpsys battery reset >/dev/null 2>&1
  if [ -n "$OLD_LOGSIZE" ]; then logcat -G "$OLD_LOGSIZE" >/dev/null 2>&1; fi
  if [ -n "$OLD_ACCESSIBILITY" ] && [ "$OLD_ACCESSIBILITY" != "null" ]; then
    settings put secure enabled_accessibility_services "$OLD_ACCESSIBILITY" >/dev/null 2>&1
  else
    settings delete secure enabled_accessibility_services >/dev/null 2>&1
  fi
  if [ -n "$OLD_ACCESSIBILITY_ENABLED" ] && [ "$OLD_ACCESSIBILITY_ENABLED" != "null" ]; then
    settings put secure accessibility_enabled "$OLD_ACCESSIBILITY_ENABLED" >/dev/null 2>&1
  fi
  # A reconnected E-ink Center service applies its profile asynchronously.
  sleep 1
  setprop vendor.debug.hwc.shim.mode "$OLD_SHIM_MODE"
  setprop vendor.debug.hwc.shim.log "$OLD_SHIM_LOG"
  setprop vendor.debug.hwc.shim.log_every "$OLD_SHIM_LOG_EVERY"
  if [ "$FORCE" = 1 ]; then setprop vendor.xrz.global_refresh_mode "$OLD_XRZ_MODE"; fi
}

stop_logcat() {
  if [ -n "$LOGCAT_PID" ]; then
    kill "$LOGCAT_PID" 2>/dev/null
    sleep 1
    if kill -0 "$LOGCAT_PID" 2>/dev/null; then kill -9 "$LOGCAT_PID" 2>/dev/null; fi
    wait "$LOGCAT_PID" 2>/dev/null
    LOGCAT_PID=
  fi
}

cleanup() {
  rc=$?
  trap - EXIT HUP INT TERM
  stop_logcat
  restore
  if [ -n "$TMP" ]; then rm -f "$TMP"; fi
  if [ "$LOCKED" = 1 ]; then
    rm -f "$LOCK/pid"
    rmdir "$LOCK" 2>/dev/null
  fi
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# mkdir is atomic on the device filesystem and is the concurrency guard. Do not
# auto-remove a stale lock: a PID-reuse race could otherwise unlock a live run.
if ! mkdir "$LOCK" 2>/dev/null; then
  owner=$(cat "$LOCK/pid" 2>/dev/null)
  cmd=
  if [ -n "$owner" ] && [ -r "/proc/$owner/cmdline" ]; then
    cmd=$(tr '\000' ' ' < "/proc/$owner/cmdline" 2>/dev/null)
  fi
  case "$cmd" in
    *measure-xproc-vendor.sh*)
      echo "another measurement is running (pid=$owner); wait for it to finish" >&2
      exit 1
      ;;
  esac
  echo "measurement lock exists at $LOCK but its owner is not running" >&2
  echo "inspect it, then remove it with: rm -rf $LOCK" >&2
  exit 1
fi
LOCKED=1
printf '%s\n' "$$" > "$LOCK/pid" || { echo "cannot write measurement lock" >&2; exit 1; }
TMP="/data/local/tmp/xproc-measure.$$.$(date +%s).log"
if ! (umask 077; set -C; : > "$TMP") 2>/dev/null; then
  echo "cannot create private measurement log: $TMP" >&2
  exit 1
fi

OLD_SHIM_MODE=$(getprop vendor.debug.hwc.shim.mode)
OLD_SHIM_LOG=$(getprop vendor.debug.hwc.shim.log)
OLD_SHIM_LOG_EVERY=$(getprop vendor.debug.hwc.shim.log_every)
OLD_XRZ_MODE=$(getprop vendor.xrz.global_refresh_mode)
OLD_STAY=$(settings get global stay_on_while_plugged_in 2>/dev/null)
OLD_LOGSIZE=$(logcat -g 2>/dev/null | sed -n 's/.*ring buffer is \([0-9][0-9]*\) *\([KMG]\)iB.*/\1\2/p' | head -1)
OLD_ACCESSIBILITY=$(settings get secure enabled_accessibility_services 2>/dev/null)
OLD_ACCESSIBILITY_ENABLED=$(settings get secure accessibility_enabled 2>/dev/null)
RESTORE_READY=1

# E-ink Center's accessibility service intentionally forces HWC mode=-1 when it
# connects and applies the foreground app's profile. Remove only that component
# for the measurement so it cannot override the requested Fast/Hyper mode.
MEASURE_ACCESSIBILITY=
if [ -n "$OLD_ACCESSIBILITY" ] && [ "$OLD_ACCESSIBILITY" != "null" ]; then
  MEASURE_ACCESSIBILITY=$(printf '%s' "$OLD_ACCESSIBILITY" | sed \
    's#com.hibreak.einkcenter/com.hibreak.einkcenter.EinkAccessibilityService##g; s#^:##; s#:$##; s#::#:#g')
fi
if [ -n "$MEASURE_ACCESSIBILITY" ]; then
  settings put secure enabled_accessibility_services "$MEASURE_ACCESSIBILITY" >/dev/null 2>&1
else
  settings delete secure enabled_accessibility_services >/dev/null 2>&1
fi
sleep 2

if [ "$FORCE" = 1 ]; then
  setprop vendor.debug.hwc.shim.mode "$MODE"
  setprop vendor.xrz.global_refresh_mode "$MODE"
else
  setprop vendor.debug.hwc.shim.mode -1
  echo "vendor mode (not forced): $(getprop vendor.xrz.global_refresh_mode)"
fi
setprop vendor.debug.hwc.shim.log 1
setprop vendor.debug.hwc.shim.log_every 1
dumpsys battery set ac 1 >/dev/null 2>&1
settings put global stay_on_while_plugged_in 7 >/dev/null 2>&1
svc power stayon true >/dev/null 2>&1
cmd power wakeup >/dev/null 2>&1
input keyevent 224 >/dev/null 2>&1
input keyevent 82 >/dev/null 2>&1
am force-stop "$PKG" >/dev/null 2>&1
if ! am start -n "$PKG/.AnimActivity"; then
  echo "failed to launch $PKG/.AnimActivity" >&2
  exit 1
fi
sleep 2
if ! pidof "$PKG" >/dev/null 2>&1; then
  echo "$PKG did not start; refusing to measure an idle screen" >&2
  exit 1
fi
sleep 4

logcat -G 16M >/dev/null 2>&1 || true
logcat -c >/dev/null 2>&1 || { echo "logcat clear failed" >&2; exit 1; }
# This GSI's logcat -d reader does not reliably terminate. Use a live reader,
# bound it to an exact timestamp window, and stop it explicitly.
logcat -v epoch -b all hwcshim:V EINKBENCH:V AndroidRuntime:V ActivityManager:V '*:S' > "$TMP" 2>&1 &
LOGCAT_PID=$!
sleep 1
if ! kill -0 "$LOGCAT_PID" 2>/dev/null; then
  echo "logcat reader failed to start" >&2
  exit 1
fi
START_MS=$(date +%s%3N)
sleep "$N"
END_MS=$(date +%s%3N)
stop_logcat

if [ ! -s "$TMP" ]; then
  echo "logcat collection produced no output; refusing to report a count" >&2
  exit 1
fi

COMMITS=$(awk -v s="$START_MS" -v e="$END_MS" '$1*1000>=s && $1*1000<=e && /commit #/ {n++} END{print n+0}' "$TMP")
FAILS=$(awk -v s="$START_MS" -v e="$END_MS" '$1*1000>=s && $1*1000<=e && /xproc vendor conversion failed/ {n++} END{print n+0}' "$TMP")
FALLBACKS=$(awk -v s="$START_MS" -v e="$END_MS" '$1*1000>=s && $1*1000<=e && /same-frame CPU fallback/ {n++} END{print n+0}' "$TMP")
QUARANTINE=$(awk -v s="$START_MS" -v e="$END_MS" '$1*1000>=s && $1*1000<=e && /quarantined destination/ {n++} END{print n+0}' "$TMP")
LAST=$(awk -v s="$START_MS" -v e="$END_MS" '$1*1000>=s && $1*1000<=e && /commit #/ {line=$0} END{if(line!="") print line}' "$TMP")
FIRST=$(awk -v s="$START_MS" -v e="$END_MS" '$1*1000>=s && $1*1000<=e && /commit #/ {print; exit}' "$TMP")
CONVERSIONS=$(awk -v s="$START_MS" -v e="$END_MS" '$1*1000>=s && $1*1000<=e && /xproc vendor convert\+raw-copy/ {n++} END{print n+0}' "$TMP")
CONV=$(awk -v s="$START_MS" -v e="$END_MS" '$1*1000>=s && $1*1000<=e && /xproc vendor convert\+raw-copy/ {line=$0} END{if(line!="") print line}' "$TMP")
APP=$(awk -v s="$START_MS" -v e="$END_MS" '$1*1000>=s && $1*1000<=e && /EINKBENCH/ {print}' "$TMP" | tail -5)
CRASHES=$(awk -v s="$START_MS" -v e="$END_MS" '$1*1000>=s && $1*1000<=e && /FATAL EXCEPTION|AndroidRuntime/ {n++} END{print n+0}' "$TMP")
FPS=$(awk -v c="$COMMITS" -v n="$N" 'BEGIN{printf "%.2f", c/n}')

echo "seconds=$N mode=$MODE physical_vendor_commits=$COMMITS conversions=$CONVERSIONS commit_fps=$FPS fallbacks=$FAILS same_frame_fallbacks=$FALLBACKS quarantines=$QUARANTINE crashes=$CRASHES"
echo "app_choreographer_fps:"
if [ -n "$APP" ]; then echo "$APP"; else echo "(no EINKBENCH lines captured)"; fi
echo "first_commit=$FIRST"
echo "last_commit=$LAST"
echo "last_conversion=$CONV"

if [ "$COMMITS" -eq 0 ]; then
  echo "no physical vendor commits captured; measurement failed" >&2
  exit 1
fi
if [ -z "$APP" ]; then
  echo "no EINKBENCH frame logs captured; animation was not confirmed" >&2
  exit 1
fi
if [ "$CRASHES" -ne 0 ]; then
  echo "AndroidRuntime errors captured; measurement failed" >&2
  exit 1
fi
exit 0
