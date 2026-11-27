#!/usr/bin/env bash
# Re-sign a GSI system image with an AVB hashtree footer suitable for DSU.
#
# Why this exists: the previous attempt (gsi/system-testkey.img) produced a footer
# with an EMPTY hashtree "Partition Name".  fs_mgr looks descriptors up by name
# (logical_partition_name == "system" for a DSU /system), so that image can never
# mount, whatever key it was signed with.  This script always sets
# --partition_name system and verifies the result afterwards.
#
# Usage:
#   tools/resign-gsi.sh -i gsi/system-td-arm64-vanilla.img \
#                       -k gsi/testkey.pk8 -o gsi/system-signed.img [-a SHA256_RSA2048] [--no-fec]
#
# The output image size is whatever avbtool produces (no --partition_size padding),
# because DSU requires KEY_SYSTEM_SIZE to equal the exact raw image size.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"
AVBTOOL="${AVBTOOL:-$ROOT/gsi/avbtool.py}"

IMG="" KEY="" OUT="" ALGO="SHA256_RSA2048" FEC=1 ROLLBACK=0
while [ $# -gt 0 ]; do
    case "$1" in
        -i|--image)   IMG="$2"; shift 2 ;;
        -k|--key)     KEY="$2"; shift 2 ;;
        -o|--output)  OUT="$2"; shift 2 ;;
        -a|--algo)    ALGO="$2"; shift 2 ;;
        -r|--rollback) ROLLBACK="$2"; shift 2 ;;
        --no-fec)     FEC=0; shift ;;
        -h|--help)    sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[ -n "$IMG" ] || { echo "missing -i/--image" >&2; exit 2; }
[ -n "$KEY" ] || { echo "missing -k/--key"   >&2; exit 2; }
[ -n "$OUT" ] || { echo "missing -o/--output" >&2; exit 2; }
for f in "$IMG" "$KEY" "$AVBTOOL"; do
    [ -e "$f" ] || { echo "not found: $f" >&2; exit 1; }
done

python_bin="${PYTHON:-python3}"
avb() { "$python_bin" "$AVBTOOL" "$@"; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "==> stripping existing footer from $(basename "$IMG")"
# erase_footer restores the original (pre-hashtree) image exactly; it works in
# place, so operate on a copy.
cp --reflink=auto "$IMG" "$work/system.raw.img"
avb erase_footer --image "$work/system.raw.img" >/dev/null
raw_size=$(stat -c %s "$work/system.raw.img")
echo "    raw image: $raw_size bytes"

echo "==> signing with $(basename "$KEY") ($ALGO)"
args=(add_hashtree_footer
      --image "$work/system.raw.img"
      --partition_name system
      --hash_algorithm sha256
      --key "$KEY"
      --algorithm "$ALGO"
      --rollback_index "$ROLLBACK"
      --rollback_index_location 0
      --append_to_release_string " dsu-resign")
[ "$FEC" -eq 1 ] || args+=(--do_not_generate_fec)
if [ "$FEC" -eq 1 ] && ! command -v fec >/dev/null 2>&1; then
    echo "    note: 'fec' binary not found, signing without FEC (optional for DSU)"
    args+=(--do_not_generate_fec)
fi
avb "${args[@]}" >/dev/null

mv "$work/system.raw.img" "$OUT"

echo "==> verifying $OUT"
info=$(avb info_image --image "$OUT")
echo "$info" | sed -n '1,12p'

fail=0
grep -q "Partition Name:        system$" <<<"$info" || { echo "!! hashtree Partition Name is not 'system'" >&2; fail=1; }
grep -q "Flags:                    0$"    <<<"$info" || { echo "!! unexpected vbmeta flags" >&2; fail=1; }
sha=$(awk -F': *' '/Public key \(sha1\)/{print $2; exit}' <<<"$info")
echo "    key sha1:   $sha"
echo "    final size: $(stat -c %s "$OUT") bytes"
[ "$fail" -eq 0 ] || exit 1
echo "==> OK"
