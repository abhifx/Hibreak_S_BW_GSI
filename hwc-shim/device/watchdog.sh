#!/system/bin/sh
# Dead-man's switch for display-stack experiments.
#
#   watchdog.sh [delay_seconds]
#
# A reboot is the only way back if SurfaceFlinger crash-loops, because the
# framework then never reaches the point where adbd is served. "Healthy" means
# system_server is present AND still the same pid 10 s later (i.e. not
# crash-looping), with SF and the composer alive.
sleep ${1:-90}
i=0
while [ $i -lt 4 ]; do
    ss=$(pidof system_server)
    sf=$(pidof surfaceflinger)
    cp=$(pidof android.hardware.graphics.composer@2.1-service)
    sleep 10
    ss2=$(pidof system_server)
    if [ -n "${ss:-}" ] && [ "$ss" = "${ss2:-}" ] && [ -n "${sf:-}" ] && [ -n "${cp:-}" ]; then
        log -t hwcshim-wd "healthy: ss=$ss sf=$sf composer=$cp"
        exit 0
    fi
    i=$((i + 1))
done
log -t hwcshim-wd "unhealthy: ss=${ss:-none}->${ss2:-none} sf=${sf:-none} composer=${cp:-none} -> reboot"
sync
reboot
