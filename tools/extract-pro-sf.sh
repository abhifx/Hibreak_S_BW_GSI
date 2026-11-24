#!/bin/sh
# extract-pro-sf.sh -- pull SurfaceFlinger (and its private libs) out of a
# HiBreak Pro LineageOS release, without building anything.
#
#   Linux:
#     ./tools/extract-pro-sf.sh lineage-23.2-hibreakpro-1.0.3-vanilla.zip
#
# Produces ./pro-sf/ with:
#   surfaceflinger          the patched binary
#   lib64/*.so              any NEEDED libs that are not in our GSI (if present)
#   needed.txt              the DT_NEEDED list
#
# Space: the zip is ~1 GB; the extracted system.img is 2-4 GB. Delete
# pro-sf/_img after this script finishes to reclaim it.
set -eu

ZIP=${1:?usage: extract-pro-sf.sh <lineage zip> [outdir]}
OUT=${2:-pro-sf}
mkdir -p "$OUT"
IMG="$OUT/_img/system.img"

echo "==> unzip system.img"
rm -f "$IMG"
unzip -o "$ZIP" 'system.img' -d "$OUT/_img" >/dev/null

echo "==> unsparse if needed"
if command -v simg2img >/dev/null 2>&1; then
    simg2img "$IMG" "$OUT/_img/system.raw.img" 2>/dev/null && IMG="$OUT/_img/system.raw.img" || true
fi

echo "==> dump surfaceflinger"
# system-as-root images expose /system/...; older ones /bin/...
for p in /system/bin/surfaceflinger /bin/surfaceflinger; do
    if debugfs -R "dump $p $OUT/surfaceflinger" "$IMG" >/dev/null 2>&1; then
        echo "    from $p"
        break
    fi
done
[ -s "$OUT/surfaceflinger" ] || { echo "surfaceflinger not found in $IMG" >&2; exit 1; }

echo "==> dependencies"
readelf -d "$OUT/surfaceflinger" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p' | tee "$OUT/needed.txt"

# Framework libs that may be version-coupled to SF; copy the ones present.
mkdir -p "$OUT/lib64"
for lib in $(cat "$OUT/needed.txt"); do
    for p in "/system/lib64/$lib" "/lib64/$lib"; do
        if debugfs -R "dump $p $OUT/lib64/$lib" "$IMG" >/dev/null 2>&1; then
            echo "    extracted $lib"
            break
        fi
    done
done

echo
echo "==> done: $OUT/surfaceflinger ($(wc -c < "$OUT/surfaceflinger") bytes)"
echo "    delete $OUT/_img to reclaim several GB"
echo "    build/device properties of this ROM:"
debugfs -R "dump /system/build.prop $OUT/build.prop" "$IMG" >/dev/null 2>&1 && \
    grep -E 'ro.build.(fingerprint|version.sdk|version.release)' "$OUT/build.prop" || true
