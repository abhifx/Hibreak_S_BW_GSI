#!/system/bin/sh
# Dead-man's switch for display-stack experiments.
#
#   watchdog.sh [delay_seconds]
#
# A reboot is the only way back if SurfaceFlinger crash-loops, because the
# framework then never reaches the point where adbd is served. "Healthy" means
# system_server is present AND still the same pid 10 s later (i.e. not
# crash-looping), with SF and the composer alive.
delay=${1:-90}
case "$delay" in
    ''|*[!0-9]*) log -t hwcshim-wd "invalid watchdog delay '$delay'"; exit 2 ;;
esac
if [ "$delay" -lt 5 ] || [ "$delay" -gt 600 ]; then
    log -t hwcshim-wd "watchdog delay out of range: $delay"
    exit 2
fi
if ! grep -q " /vendor/lib64/hw " /proc/mounts; then
    log -t hwcshim-wd "warning: shim bind mount is not active"
fi
sleep "$delay"
i=0
while [ $i -lt 4 ]; do
    ss=$(pidof system_server)
    sf=$(pidof surfaceflinger)
    cp=$(pidof android.hardware.graphics.composer@2.1-service)
    sleep 10
    ss2=$(pidof system_server)
    sf2=$(pidof surfaceflinger)
    cp2=$(pidof android.hardware.graphics.composer@2.1-service)
    if [ -n "${ss:-}" ] && [ "$ss" = "${ss2:-}" ] &&
       [ -n "${sf:-}" ] && [ "$sf" = "${sf2:-}" ] &&
       [ -n "${cp:-}" ] && [ "$cp" = "${cp2:-}" ]; then
        log -t hwcshim-wd "healthy: ss=$ss sf=$sf composer=$cp"
        exit 0
    fi
    i=$((i + 1))
done
log -t hwcshim-wd "unhealthy: ss=${ss:-none}->${ss2:-none} sf=${sf:-none}->${sf2:-none} composer=${cp:-none}->${cp2:-none} -> reboot"
sync
reboot
