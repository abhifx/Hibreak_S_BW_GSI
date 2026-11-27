#!/usr/bin/env bash
# Read-only inspection: what AVB keys does this HiBreak actually trust for DSU?
#
#   /system must be signed by a key whose .avbpubkey blob lives in the *stock*
#   ramdisk /avb directory (TransformFstabForDsu() sets avb_keys="/avb"), and a
#   locked bootloader refuses unknown keys (IsAvbPermissive() == false).
#   So the ramdisk key list decides which GSIs can ever boot here.
#
# Nothing is written to the phone.  boot_a/boot_b are read via MTK BROM which is
# a plain read; it only happens with --dump.
#
# Usage:
#   tools/inspect-avb-keys.sh             # live state only (adb)
#   tools/inspect-avb-keys.sh --dump      # + read boot_a/boot_b over BROM and list /avb keys
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
MTK="$ROOT/mtkclient/mtk.py"
PY="$ROOT/.venv-mtk/bin/python"
OUT="$ROOT/backup/brom"
DUMP=0
[ "${1:-}" = "--dump" ] && DUMP=1

step() { printf '==> %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }
sh_()  { adb shell "$@" | tr -d '\r'; }

if ! adb devices | awk 'NR>1 && $2=="device"' | grep -q .; then
    if adb devices | grep -q unauthorized; then
        echo "Device is 'unauthorized'. Accept the 'Allow USB debugging' prompt on the phone." >&2
    fi
    exit 1
fi

step "Live state"
note "model:       $(sh_ getprop ro.product.model)"
note "fingerprint: $(sh_ getprop ro.build.fingerprint)"
note "slot:        $(sh_ getprop ro.boot.slot_suffix)"
note "locked:      $(sh_ getprop ro.boot.flash.locked)  (1 = locked)"
note "vbmeta dgst: $(sh_ getprop ro.boot.vbmeta.digest)"
note "avb version: $(sh_ getprop ro.boot.avb_version)"
note "gsi status:  $(sh_ gsi_tool status)"
note "data free:   $(sh_ df -h /data | tail -1 | awk '{print $4}')"
note "metadata:    $(sh_ ls -l /dev/block/by-name/metadata 2>/dev/null | awk '{print $NF}')"

step "System fstab as mounted (this is what DSU rewrites)"
sh_ 'cat /vendor/etc/fstab.* 2>/dev/null || cat /first_stage_ramdisk/first_stage_ramdisk/fstab.* 2>/dev/null' | grep -E '^\s*(system|vendor|product|system_ext|metadata)\s' || true

if [ "$DUMP" -eq 0 ]; then
    echo
    echo "Re-run with --dump to read boot_a/boot_b over BROM (read-only) and list the"
    echo "trusted /avb keys.  That needs 'adb reboot edl' and ~2 minutes."
    exit 0
fi

[ -x "$PY" ] || { echo "mtk python env missing: $PY" >&2; exit 1; }
mkdir -p "$OUT"

step "Rebooting to EDL"
adb reboot edl || true
sleep 8

for part in boot_a boot_b; do
    [ -f "$OUT/$part.img" ] && { note "have $OUT/$part.img, skipping"; continue; }
    step "Reading $part (read-only)"
    # mtkclient waits for the BROM/preloader port; it never writes in 'r' mode.
    RUN=("$PY" "$MTK" r "$part" "$OUT/$part.img")
    [ "$(id -u)" -eq 0 ] || RUN=(sudo -n "${RUN[@]}")
    "${RUN[@]}" || {
        echo "read failed; replug while holding nothing, or retry 'adb reboot edl'" >&2; }
done

step "Extracting ramdisk /avb key list"
for part in boot_a boot_b; do
    img="$OUT/$part.img"
    [ -f "$img" ] || continue
    d="$OUT/$part"
    rm -rf "$d"; mkdir -p "$d"
    unpack_bootimg --boot_img "$img" --out "$d" >/dev/null 2>&1 || { note "unpack_bootimg failed on $part"; continue; }
    ram="$(find "$d" -name 'ramdisk*' | head -1)"
    [ -n "$ram" ] || continue
    cd "$d"
    case "$(file -b "$ram")" in
        *gzip*)  gunzip -c "$ram" > initrd.cpio ;;
        *LZ4*)   lz4 -dc "$ram"    > initrd.cpio ;;
        *Zstandard*) zstd -dc "$ram" > initrd.cpio ;;
        *)       note "unknown ramdisk compression: $(file -b "$ram")"; continue ;;
    esac
    mkdir -p rd && (cd rd && cpio -idmu --quiet < ../initrd.cpio 2>/dev/null || true)
    cd - >/dev/null
    echo "--- $part /avb:"
    if [ -d "$d/rd/avb" ]; then
        for k in "$d/rd/avb"/*; do
            [ -f "$k" ] || continue
            note "$(basename "$k")  sha1=$(sha1sum "$k" | cut -c1-40)  $(stat -c %s "$k") bytes"
        done
    else
        note "(no /avb directory in ramdisk - avb_keys=/avb will fail for any GSI)"
    fi
    echo "--- $part cmdline:"
    tr ' ' '\n' < "$d"/cmdline 2>/dev/null | grep -Ei 'avb|verified|androidboot' | head -20 || true
done

step "Done. Compare the sha1s with: python3 gsi/avbtool.py info_image --image <gsi>"
