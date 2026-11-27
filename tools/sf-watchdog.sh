#!/system/bin/sh
# Fail-closed watchdog for a SurfaceFlinger-only bind-mount test.
# Args: expected candidate SHA-256, original SF SHA-256 (for logging only).

CANDIDATE_HASH="$1"
ORIGINAL_HASH="$2"
DIR=/data/local/tmp/sfcandidate
START=$(date +%s)
FAILS=0

echo "watchdog started candidate=$CANDIDATE_HASH original=$ORIGINAL_HASH"
echo ready > "$DIR/watchdog.ready"

# The host creates this only after the bind mount succeeds.
WAIT=0
while [ ! -f "$DIR/mounted" ]; do
    if [ "$WAIT" -ge 20 ]; then
        echo "no mount signal; watchdog exiting without changing the system"
        exit 0
    fi
    sleep 1
    WAIT=$((WAIT + 1))
done

revert() {
    echo "watchdog: reverting candidate SurfaceFlinger"
    umount /system/bin/surfaceflinger 2>/dev/null || true
    sync
    /system/bin/reboot
}

while [ $(( $(date +%s) - START )) -lt 120 ]; do
    SF_PID=$(pidof surfaceflinger 2>/dev/null)
    SS_PID=$(pidof system_server 2>/dev/null)
    LIVE_HASH=""
    if [ -n "$SF_PID" ]; then
        LIVE_HASH=$(sha256sum "/proc/$SF_PID/exe" 2>/dev/null | awk '{print $1}')
    fi
    if dumpsys SurfaceFlinger >/dev/null 2>&1; then
        DUMP_OK=1
    else
        DUMP_OK=0
    fi

    if [ -n "$SF_PID" ] && [ -n "$SS_PID" ] && [ "$LIVE_HASH" = "$CANDIDATE_HASH" ] && [ "$DUMP_OK" = "1" ]; then
        FAILS=0
    else
        FAILS=$((FAILS + 1))
        echo "unhealthy sf=$SF_PID ss=$SS_PID hash=$LIVE_HASH dump=$DUMP_OK fails=$FAILS"
    fi

    if [ "$FAILS" -ge 5 ]; then
        revert
        exit 1
    fi
    sleep 2
done

if [ "$FAILS" -gt 0 ]; then
    revert
    exit 1
fi

echo "watchdog: candidate stable for 120 seconds; disarming"
echo done > "$DIR/watchdog.done"
