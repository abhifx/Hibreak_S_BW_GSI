#!/usr/bin/env bash
# Retry loop: write the stock boot_b + vbmeta_b back over BROM.
#
# Needed when the phone is stuck in preloader/DA mode and adb is unavailable:
# mtkclient keeps retrying until the BROM/preloader handshake succeeds, e.g.
# during the brief USB window of a failed boot.
#
#   tools/recover-stock.sh            # 40 attempts, ~4 s apart
#   TRIES=200 tools/recover-stock.sh
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BROM="$ROOT/backup/brom"
MTK="$ROOT/mtkclient/mtk.py"
PY="${MTK_PY:-$ROOT/.venv-mtk/bin/python}"
LOG="$BROM/recover.log"
TRIES="${TRIES:-40}"

say() { printf '\n=== %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }

[ -x "$PY" ] || die "python venv missing: $PY"
[ -f "$MTK" ] || die "mtkclient missing: $MTK"
( cd "$BROM" && sha256sum --quiet -c SHA256SUMS ) || die "stock dumps failed checksum"

say "restoring stock boot_b + vbmeta_b (nothing else is written)"
note "stocks: $(sha256sum "$BROM/boot_b.img" | cut -d' ' -f1) boot_b"
note "        $(sha256sum "$BROM/vbmeta_b.img" | cut -d' ' -f1) vbmeta_b"
note "log:    $LOG"
note "Force the phone off now (hold power ~20 s), then let it try to boot."
note "This loop will catch the BROM window and restore the stock images."

: > "$LOG"
attempt_log="$BROM/recover.attempt.log"
for i in $(seq 1 "$TRIES"); do
    printf '\n--- attempt %d/%d %s\n' "$i" "$TRIES" "$(date +%H:%M:%S)"
    : > "$attempt_log"
    # Drop any stale DA state so a fresh BROM session uploads the DA instead of
    # calling reinit() against a loader that is no longer resident.
    rm -f "$ROOT/.state" "$ROOT/mtkclient/.state"
    "$PY" "$MTK" multi "w boot_b,vbmeta_b $BROM/boot_b.img,$BROM/vbmeta_b.img;reset" 2>&1 \
        | tee -a "$attempt_log" >> "$LOG" \
        | grep -E 'Wrote |Handshake failed|ERROR|Traceback|Reset command' || true
    if grep -q 'Wrote .*boot_b' "$attempt_log" && \
       grep -q 'Wrote .*vbmeta_b' "$attempt_log"; then
        say "STOCK RESTORED - both partitions written"
        note "Power-cycle the phone; it should boot stock Android."
        exit 0
    fi
    sleep 2
done

say "no BROM session after $TRIES attempts"
note "Check the log: $LOG"
note "Try: unplug USB, hold power 20 s, then hold Vol Up + Vol Down while plugging in,"
note "or start this script again and power the phone on."
exit 1
