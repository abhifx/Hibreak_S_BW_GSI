#!/system/bin/sh
# Keep the optional GPU gray helper alive while xproc is enabled.
# The shim clears helper_recovery=1 after repeated conversion stalls; that is a
# stronger signal than "a process named gray_helper exists", which a helper hung
# in EGL/gralloc/kernel I/O also satisfies.
D=/data/local/tmp/hwc-shim-mt6765
PIDFILE=$D/gray-helper.pid

is_helper() {
    pid=$1
    [ -n "$pid" ] || return 1
    [ -r "/proc/$pid/cmdline" ] || return 1
    tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q 'gray_helper'
}

start_helper() {
    # The old process may be hung in an uninterruptible driver call. SIGKILL is
    # the only recovery that does not depend on the helper's own event loop.
    old=$(cat "$PIDFILE" 2>/dev/null)
    if is_helper "$old"; then kill -9 "$old" 2>/dev/null; fi
    pkill -9 -x gray_helper 2>/dev/null
    rm -f "$PIDFILE"
    nohup "$D/gray_helper" --connect hwcgray_hwc >>"$D/gray-helper.log" 2>&1 </dev/null &
    pid=$!
    echo "$pid" > "$PIDFILE"
    sleep 2
    if is_helper "$pid"; then
        setprop vendor.debug.hwc.shim.helper_recovery 0
        log -t hwcshim-wd "gray_helper ready pid=$pid"
        return 0
    fi
    rm -f "$PIDFILE"
    log -t hwcshim-wd "gray_helper start failed pid=$pid"
    return 1
}

reap_empty_cgroups() {
    # One find process plus batched rmdir calls. A per-directory shell loop forks
    # thousands of short-lived processes, creating the same pid cgroups we reap.
    find /sys/fs/cgroup -mindepth 3 -maxdepth 3 -type d -name 'pid_*' -exec rmdir {} + 2>/dev/null
}

tick=0
while [ "$(getprop vendor.debug.hwc.shim.xproc)" = "1" ]; do
    if [ "$(getprop vendor.debug.hwc.shim.helper_recovery)" = "1" ]; then
        log -t hwcshim-wd "shim requested helper recovery"
        start_helper
    else
        pid=$(cat "$PIDFILE" 2>/dev/null)
        if ! is_helper "$pid"; then
            existing=$(pidof gray_helper 2>/dev/null | awk '{print $1}')
            if is_helper "$existing"; then
                echo "$existing" > "$PIDFILE"
            else
                start_helper
            fi
        fi
    fi
    # Empty per-PID cgroup2 directories pin ~900 KB of SUnreclaim each on this
    # kernel. Reap every 30 s, not every helper-health tick.
    tick=$((tick + 1))
    if [ $((tick % 6)) -eq 0 ]; then
        reap_empty_cgroups
    fi
    sleep 5
done
