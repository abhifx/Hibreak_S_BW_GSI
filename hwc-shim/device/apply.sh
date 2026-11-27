#!/system/bin/sh
# Install the mt6765 HWC shim into /vendor/lib64/hw and reload the display stack.
#
#   apply.sh <variant|replace|none> [watchdog_seconds]
#
# The composer's linker namespace permits only /odm, /vendor and /system/vendor, so
# /system/xbin cannot be dlopened from; the whole module directory is copied to
# tmpfs and bind-mounted back instead.
#
# CRITICAL: per-file SELinux labels must be preserved. A blanket chcon to
# vendor_file relabels gralloc.rogue.so / gralloc.default.so /
# android.hardware.graphics.mapper@4.0-impl-mediatek.so / vulkan.mt6765.so away from
# same_process_hal_file, after which SurfaceFlinger aborts at startup with
# "no suitable EGLConfig found" and the framework never comes back.

LAYOUT=${1:-variant}
WAIT=${2:-90}
SRC=/vendor/lib64/hw
M=/system/xbin/hwmirror
D=/data/local/tmp/hwc-shim-mt6765

case "$LAYOUT" in
    variant|replace|none) ;;
    *) echo "unknown layout $LAYOUT"; exit 2 ;;
esac
case "$WAIT" in
    ''|*[!0-9]*) echo "watchdog delay must be an integer"; exit 2 ;;
esac
if [ "$WAIT" -lt 5 ] || [ "$WAIT" -gt 600 ]; then
    echo "watchdog delay must be 5..600 seconds"; exit 2
fi
if [ "$(getenforce 2>/dev/null)" != "Enforcing" ]; then
    echo "refusing: SELinux is not Enforcing"; exit 1
fi
if grep -q " $M " /proc/mounts; then
    echo "refusing: $M is still a mountpoint"; exit 1
fi

ctx_of() {
    ls -ldZ "$1" 2>/dev/null | awk '{print $5}'
}
copy_labeled() {
    src=$1
    dst=$2
    ctx=$(ctx_of "$src")
    if [ -z "$ctx" ]; then echo "missing SELinux context: $src"; exit 1; fi
    if [ -L "$src" ]; then
        # Label the link itself; its relative target may not have been copied
        # yet, and following a dangling link would fail with ENOENT.
        chcon -h "$ctx" "$dst" || { echo "chcon -h failed: $dst <- $ctx"; exit 1; }
    else
        chcon "$ctx" "$dst" || { echo "chcon failed: $dst <- $ctx"; exit 1; }
    fi
    got=$(ctx_of "$dst")
    if [ "$got" != "$ctx" ]; then
        echo "label verification failed: $dst got '$got' expected '$ctx'"; exit 1
    fi
}

umount $SRC 2>/dev/null
# A plain umount can fail with EBUSY while the composer still has the old files
# mapped; -l detaches the mount from the namespace, which is all we need.
if grep -q " $SRC " /proc/mounts; then umount -l $SRC 2>/dev/null; fi
if grep -q " $SRC " /proc/mounts; then echo "could not detach $SRC"; exit 1; fi

# Never delete through a live mount: doing that once emptied the module directory.
n=$(ls $SRC 2>/dev/null | wc -l)
if [ "$n" -lt 40 ]; then echo "refusing: $SRC shows only $n files"; exit 1; fi

pkill -f 'hwc-shim-mt6765/watchdog.sh' 2>/dev/null
rm -rf $M
mkdir -p $M

n=0
for f in $SRC/*; do
    b=$(basename "$f")
    cp -a "$f" "$M/$b" || { echo "copy failed: $b"; exit 1; }
    copy_labeled "$f" "$M/$b"
    n=$((n + 1))
done
copy_labeled "$SRC" "$M"

for b in gralloc.rogue.so gralloc.default.so \
         android.hardware.graphics.mapper@4.0-impl-mediatek.so vulkan.mt6765.so; do
    if [ ! -e "$SRC/$b" ]; then echo "missing critical vendor module: $b"; exit 1; fi
    ctx=$(ctx_of "$M/$b")
    if [ -z "$ctx" ]; then echo "critical module has no context: $b"; exit 1; fi
done

case "$LAYOUT" in
    variant)
        cp $D/hwcshim.mt6765.so $M/hwcomposer.mtk_common.so || exit 1
        # The composer service is not root. Do not inherit a restrictive umask
        # from an E-ink Center su shell when persistence runs this at boot.
        chmod 644 $M/hwcomposer.mtk_common.so || exit 1
        chcon u:object_r:vendor_file:s0 $M/hwcomposer.mtk_common.so || {
            echo "chcon failed: shim"; exit 1; }
        [ "$(ctx_of $M/hwcomposer.mtk_common.so)" = "u:object_r:vendor_file:s0" ] || {
            echo "shim label verification failed"; exit 1; }
        ;;
    replace)
        realctx=$(ctx_of "$M/hwcomposer.mt6765.so")
        if [ -z "$realctx" ]; then echo "missing original composer context"; exit 1; fi
        [ -f $M/hwcomposer.mt6765.real.so ] || cp -a $M/hwcomposer.mt6765.so $M/hwcomposer.mt6765.real.so
        chcon "$realctx" $M/hwcomposer.mt6765.real.so || {
            echo "real composer label restore failed"; exit 1; }
        cp $D/hwcshim.mt6765.so $M/hwcomposer.mt6765.so || exit 1
        chmod 644 $M/hwcomposer.mt6765.so || exit 1
        chcon u:object_r:vendor_file:s0 $M/hwcomposer.mt6765.so || {
            echo "chcon failed: shim"; exit 1; }
        [ "$(ctx_of $M/hwcomposer.mt6765.so)" = "u:object_r:vendor_file:s0" ] || {
            echo "shim label verification failed"; exit 1; }
        [ "$(ctx_of $M/hwcomposer.mt6765.real.so)" = "$realctx" ] || {
            echo "real composer label verification failed"; exit 1; }
        ;;
    none) : ;;
esac

mount --bind $M $SRC || { echo "mount failed"; exit 1; }
grep -q " $SRC " /proc/mounts || { echo "bind mount is not visible"; exit 1; }

echo "layout=$LAYOUT files=$n"
ls -lZ $M | grep -E "gralloc|vulkan|mapper|hwcomposer" | awk '{print "  " $5, $9}'

SS0=$(pidof system_server)
nohup sh $D/watchdog.sh "$WAIT" >/dev/null 2>&1 &
wd=$!
sleep 1
if ! kill -0 "$wd" 2>/dev/null; then
    echo "watchdog failed to start"; exit 1
fi

# Preserve the first pre-deployment e-ink sysfs state for same-boot -Revert.
STATE=$D/eink-debug-before.env
if [ ! -f "$STATE" ]; then
    : > "$STATE"
    for f in clean_a2 anti_flicker; do
        p=/sys/kernel/debug/eink_debug/$f
        if [ -e "$p" ]; then
            printf '%s=%s\n' "$f" "$(cat $p 2>/dev/null)" >> "$STATE"
        fi
    done
fi

# Kernel e-ink knobs that the stock framework initialises but a GSI does not.
# From the HiBreak Pro LineageOS device tree (init.hibreakpro.eink.rc):
# clean_a2 gates the kernel's fast whole-panel DU/A2 path -- off, the same
# update takes roughly twice as long. Guarded: the node may not exist.
for f in clean_a2 anti_flicker; do
    p=/sys/kernel/debug/eink_debug/$f
    if [ -e "$p" ]; then
        echo "eink_debug $f: $(cat $p 2>/dev/null) -> 1"
        echo 1 > "$p" 2>/dev/null || echo "warning: could not write $p"
    fi
done

logcat -c
# The composer must be reloaded to pick up the new module. SurfaceFlinger dies
# with it and init restarts it, which is fine -- but the Java side cannot survive
# a stale SurfaceControl state: system_server dies ~90 s later with cascading
# WM Shell / launcher crashes ("InputChannel is not initialized",
# "mNativeObject of Surface ... is null"). Restart zygote deliberately instead,
# which also leaves adbd untouched.
setprop ctl.restart vendor.hwcomposer-2-1
sleep 3
setprop ctl.restart zygote

i=0
while [ $i -lt 20 ]; do
    cp=$(pidof android.hardware.graphics.composer@2.1-service)
    sf=$(pidof surfaceflinger)
    if [ -n "$cp" ] && [ -n "$sf" ]; then break; fi
    sleep 1
    i=$((i + 1))
done
if [ -z "${cp:-}" ] || [ -z "${sf:-}" ]; then
    echo "display stack did not come back (sf=${sf:-none} composer=${cp:-none})"
    exit 1
fi

echo "ss=$(pidof system_server) sf=$sf composer=$cp"
echo "watchdog: system_server was $SS0 (a zygote restart is expected to change it)"
