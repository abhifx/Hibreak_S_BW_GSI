#!/system/bin/sh
# Reapply the deployed HWC/helper overlay after a DSU reboot.
#
# Enabled by deploy.ps1. deploy.ps1 -Revert creates persist.disabled, which makes
# this script a no-op across reboots. The overlay remains a bind mount; no
# partition, boot image, kernel, or DSU userdata is modified.
D=/data/local/tmp/hwc-shim-mt6765
umask 022
LOCK=$D/persist.lock
LOG=$D/persist.log
EXPECTED_VENDOR=51827cc37c89dc4aef99b0da39f5200e13fa92014c1b7d04c6b41ff4c26200c6

exec >>"$LOG" 2>&1
log() { echo "$(date '+%m-%d %H:%M:%S') $*"; }

if [ -f "$D/persist.disabled" ]; then
    log "persistence disabled by persist.disabled"
    exit 0
fi
if [ ! -f "$D/persist.env" ] || [ ! -f "$D/persist.layout" ] || [ ! -f "$D/persist.watchdog" ]; then
    log "persistent profile missing; run deploy.ps1"
    exit 1
fi

BOOT_ID=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)
if [ -n "$BOOT_ID" ] && [ -f "$D/persist.boot" ] && \
   [ "$(cat "$D/persist.boot" 2>/dev/null)" = "$BOOT_ID" ] && \
   grep -q " /vendor/lib64/hw " /proc/mounts; then
    log "overlay already active for boot $BOOT_ID"
    exit 0
fi

if ! mkdir "$LOCK" 2>/dev/null; then
    owner=$(cat "$LOCK/pid" 2>/dev/null)
    cmd=
    if [ -n "$owner" ] && [ -r "/proc/$owner/cmdline" ]; then
        cmd=$(tr '\000' ' ' < "/proc/$owner/cmdline" 2>/dev/null)
    fi
    case "$cmd" in
        *hwc-shim-mt6765/persist.sh*)
            log "another persistence run is active (pid=$owner)"
            exit 0
            ;;
    esac
    log "removing stale persistence lock"
    rm -rf "$LOCK"
    mkdir "$LOCK" 2>/dev/null || { log "cannot create persistence lock"; exit 1; }
fi
printf '%s\n' "$$" > "$LOCK/pid"
cleanup() {
    rc=$?
    trap - EXIT HUP INT TERM
    rm -f "$LOCK/pid"
    rmdir "$LOCK" 2>/dev/null
    exit "$rc"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

log "starting persistence for boot $BOOT_ID"
if [ "$(getenforce 2>/dev/null)" != "Enforcing" ]; then
    log "refusing: SELinux is not Enforcing"
    exit 1
fi
for f in hwcshim.mt6765.so apply.sh watchdog.sh; do
    if [ ! -f "$D/$f" ]; then log "missing $f"; exit 1; fi
done
vendor_hash=$(sha256sum /vendor/lib64/hw/hwcomposer.mt6765.so 2>/dev/null | awk '{print $1}')
if [ "$vendor_hash" != "$EXPECTED_VENDOR" ]; then
    log "vendor ABI mismatch: $vendor_hash"
    exit 1
fi

while IFS='=' read -r key value; do
    [ -n "$key" ] || continue
    setprop "$key" "$value"
done < "$D/persist.env"
setprop vendor.debug.hwc.shim.helper_recovery 0

if [ "$(getprop vendor.debug.hwc.shim.xproc)" = "1" ]; then
    if [ ! -f "$D/gray_helper" ] || [ ! -f "$D/helper-watchdog.sh" ]; then
        log "helper files missing"
        exit 1
    fi
    if ! pidof gray_helper >/dev/null 2>&1; then
        nohup "$D/gray_helper" --connect hwcgray_hwc >>"$D/gray-helper.log" 2>&1 </dev/null &
        echo $! > "$D/gray-helper.pid"
        sleep 2
    fi
    if ! pidof gray_helper >/dev/null 2>&1; then
        log "gray_helper failed to start"
        exit 1
    fi
    if ! ps -A -o ARGS 2>/dev/null | grep -q '[h]wc-shim-mt6765/helper-watchdog.sh'; then
        nohup sh "$D/helper-watchdog.sh" >/dev/null 2>&1 </dev/null &
        echo $! > "$D/helper-watchdog.pid"
    fi
fi

if grep -q " /vendor/lib64/hw " /proc/mounts; then
    log "bind mount already active; skipping apply.sh"
else
    layout=$(cat "$D/persist.layout")
    watchdog=$(cat "$D/persist.watchdog")
    log "applying layout=$layout watchdog=$watchdog"
    if ! sh "$D/apply.sh" "$layout" "$watchdog"; then
        log "apply.sh failed"
        exit 1
    fi
fi

if ! grep -q " /vendor/lib64/hw " /proc/mounts; then
    log "bind mount not active after apply"
    exit 1
fi
# apply.sh intentionally returns as soon as composer/SF exist. A zygote restart
# can still be in flight, so do not mark the boot complete until the framework
# and SystemUI are back as well.
i=0
while { [ -z "$(pidof system_server 2>/dev/null)" ] || \
       [ -z "$(pidof com.android.systemui 2>/dev/null)" ] || \
       [ -z "$(pidof surfaceflinger 2>/dev/null)" ] || \
       [ -z "$(pidof android.hardware.graphics.composer@2.1-service 2>/dev/null)" ]; } && \
      [ $i -lt 60 ]; do
    sleep 1
    i=$((i + 1))
done
composer=$(pidof android.hardware.graphics.composer@2.1-service)
sf=$(pidof surfaceflinger)
system=$(pidof system_server)
systemui=$(pidof com.android.systemui)
if [ -z "$composer" ] || [ -z "$sf" ] || [ -z "$system" ] || [ -z "$systemui" ]; then
    log "framework/display stack missing after apply (system=$system sf=$sf composer=$composer systemui=$systemui)"
    exit 1
fi
if [ "$(getprop vendor.debug.hwc.shim.xproc)" = "1" ] && ! pidof gray_helper >/dev/null 2>&1; then
    log "helper missing after apply"
    exit 1
fi

printf '%s\n' "$BOOT_ID" > "$D/persist.boot"
log "persistence complete (system=$system sf=$sf composer=$composer systemui=$systemui helper=$(pidof gray_helper 2>/dev/null))"
