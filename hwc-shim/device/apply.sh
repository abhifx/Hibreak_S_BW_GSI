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
    ctx=$(ls -lZ "$f" 2>/dev/null | awk '{print $5}')
    if [ -n "$ctx" ]; then chcon "$ctx" "$M/$b" 2>/dev/null; fi
    n=$((n + 1))
done
dctx=$(ls -ldZ $SRC 2>/dev/null | awk '{print $5}')
[ -n "$dctx" ] && chcon "$dctx" "$M" 2>/dev/null

case "$LAYOUT" in
    variant)
        cp $D/hwcshim.mt6765.so $M/hwcomposer.mtk_common.so || exit 1
        chcon u:object_r:vendor_file:s0 $M/hwcomposer.mtk_common.so
        ;;
    replace)
        [ -f $M/hwcomposer.mt6765.real.so ] || cp -a $M/hwcomposer.mt6765.so $M/hwcomposer.mt6765.real.so
        cp $D/hwcshim.mt6765.so $M/hwcomposer.mt6765.so || exit 1
        chcon u:object_r:vendor_file:s0 $M/hwcomposer.mt6765.so $M/hwcomposer.mt6765.real.so
        ;;
    none) : ;;
    *) echo "unknown layout $LAYOUT"; exit 2 ;;
esac

mount --bind $M $SRC || { echo "mount failed"; exit 1; }

echo "layout=$LAYOUT files=$n"
ls -lZ $M | grep -E "gralloc|vulkan|mapper|hwcomposer" | awk '{print "  " $5, $9}'

SS0=$(pidof system_server)
nohup sh $D/watchdog.sh "$WAIT" >/dev/null 2>&1 &

# Kernel e-ink knobs that the stock framework initialises but a GSI does not.
# From the HiBreak Pro LineageOS device tree (init.hibreakpro.eink.rc):
# clean_a2 gates the kernel's fast whole-panel DU/A2 path -- off, the same
# update takes roughly twice as long. Guarded: the node may not exist.
for f in clean_a2 anti_flicker; do
    p=/sys/kernel/debug/eink_debug/$f
    if [ -e "$p" ]; then
        echo "eink_debug $f: $(cat $p 2>/dev/null) -> 1"
        echo 1 > "$p" 2>/dev/null
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
sleep 3

echo "ss=$(pidof system_server) sf=$(pidof surfaceflinger) composer=$(pidof android.hardware.graphics.composer@2.1-service)"
echo "watchdog: system_server was $SS0 (a zygote restart is expected to change it)"
