#!/usr/bin/env bash
# DSU helper for the Bigme HiBreak (Linux port of dsu.ps1).
#
# Installs a GSI as a Dynamic System Update: the image lives in /data as a
# logical partition, so system/vendor/boot/super are never written.
#
# gsi_tool refuses to install as non-root on this user build, so we drive
# com.android.dynsystem directly via the START_INSTALL intent. The shell uid
# holds INSTALL_DYNAMIC_SYSTEM, which works without root or an unlocked
# bootloader.
#
# Three DSU requirements this script handles:
#   1. the installer only accepts .gz/.gzip/.zip URIs (a raw .img is rejected)
#   2. KEY_SYSTEM_SIZE must be the EXACT raw image size (AVB footer at the end)
#   3. a completed install can land in 'disabled'; gsi_tool enable arms it
#
# Usage:
#   tools/dsu.sh status
#   tools/dsu.sh install [-i IMG] [--single-boot] [--reboot] [--skip-hash]
#   tools/dsu.sh enable [--single-boot] | disable | wipe | cancel | reboot
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$HERE")"

ACTION="${1:-status}"; shift || true

IMG="$ROOT/gsi/system-td-arm64-vanilla.img"
SERIAL=""
USERDATA_SIZE=8589934592
TIMEOUT_MIN=15
SINGLE_BOOT=0
DO_REBOOT=0
SKIP_HASH=0
# d06c66e4… is the published/xz download; a0704a79… is the raw image the DSU
# installer actually needs (KEY_SYSTEM_SIZE must be the raw size).
EXPECTED_XZ_SHA256="d06c66e45c6d1e8d61b45d1261a65cb7ccf2e7946739602011e500a12ae55ad6"
EXPECTED_IMG_SHA256="a0704a7909328cc077f314f25ed47c5ce02b4a095cc932dd36060e4a90d333a8"

while [ $# -gt 0 ]; do
    case "$1" in
        -i|--image)     IMG="$2"; shift 2 ;;
        -s|--serial)    SERIAL="$2"; shift 2 ;;
        -u|--userdata)  USERDATA_SIZE="$2"; shift 2 ;;
        -t|--timeout)   TIMEOUT_MIN="$2"; shift 2 ;;
        --single-boot)  SINGLE_BOOT=1; shift ;;
        --reboot)       DO_REBOOT=1; shift ;;
        --skip-hash)    SKIP_HASH=1; shift ;;
        -h|--help)      sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

step() { printf '==> %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }
warn() { printf '!!  %s\n' "$*" >&2; }

adb_() { adb ${SERIAL:+-s "$SERIAL"} "$@"; }
sh_()  { adb_ shell "$@" | tr -d '\r'; }

resolve_target() {
    local out
    out="$(adb devices | awk 'NR>1 && $2=="device"{print $1}')"
    if [ -z "$out" ]; then
        if adb devices | grep -q unauthorized; then
            echo "Device is 'unauthorized' - accept the USB debugging prompt on the phone." >&2
        else
            echo "No adb device in 'device' state. Connect the HiBreak and enable USB debugging." >&2
        fi
        exit 1
    fi
    if [ -n "$SERIAL" ]; then return; fi
    if [ "$(echo "$out" | wc -l)" -eq 1 ]; then SERIAL="$out"; return; fi
    local d m
    for d in $out; do
        m="$(adb -s "$d" shell getprop ro.product.model 2>/dev/null | tr -d '\r')"
        if [ "$m" = "HiBreak" ]; then SERIAL="$d"; return; fi
    done
    echo "Multiple adb devices ($(echo $out)). Pass --serial." >&2
    exit 1
}

gsi_status() { sh_ gsi_tool status; }

data_free_bytes() {
    local line fields
    line="$(sh_ df -k /data | tail -1)"
    read -r -a fields <<<"$line"
    echo $(( ${fields[3]} * 1024 ))
}

wait_for_install() {
    local deadline status=normal
    deadline=$(( $(date +%s) + TIMEOUT_MIN * 60 ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        sleep 5
        status="$(gsi_status)"
        note "status: $status"
        [ "$status" != "normal" ] && break
    done
    printf '%s' "$status"
}

cmd_status() {
    resolve_target
    local fp slot locked free gs
    fp="$(sh_ getprop ro.build.fingerprint)"
    slot="$(sh_ getprop ro.boot.slot_suffix)"
    locked="$(sh_ getprop ro.boot.flash.locked)"
    free="$(data_free_bytes)"
    gs="$(gsi_status)"
    step "HiBreak DSU status"
    note "build:      $fp"
    note "slot:       $slot"
    note "bootloader: $([ "$locked" = "1" ] && echo locked || echo unlocked)"
    note "data free:  $(awk -v b="$free" 'BEGIN{printf "%.1f GB", b/1073741824}')"
    note "gsi_tool:   $(echo "$gs" | paste -sd, -)"
    case "$gs" in
        *disabled*) note "DSU installed but disabled; 'enable' arms it for next boot" ;;
    esac
    case "$fp" in
        *treble*|*td-*|*aosp*) note "looks like you are running a GSI right now" ;;
    esac
}

cmd_install() {
    resolve_target
    local status model locked raw gz raw_size gz_size free needed
    status="$(gsi_status)"
    [ "$status" = "normal" ] || { echo "DSU status is '$status', not 'normal'. Run 'disable'/'wipe' first." >&2; exit 1; }

    model="$(sh_ getprop ro.product.model)"
    locked="$(sh_ getprop ro.boot.flash.locked)"
    step "Target: $model (${SERIAL})"
    if [ "$locked" = "1" ]; then
        warn "Bootloader is LOCKED. The GSI must be signed with a key present in the"
        warn "stock ramdisk /avb, otherwise first-stage mount rejects it. Stock"
        warn "partitions are not modified either way."
    fi

    [ -f "$IMG" ] || { echo "image not found: $IMG" >&2; exit 1; }
    if [ "$SKIP_HASH" -eq 0 ] && [ "$(basename "$IMG")" = "system-td-arm64-vanilla.img" ]; then
        step "Verifying SHA-256 of $(basename "$IMG")"
        local hash; hash="$(sha256sum "$IMG" | awk '{print $1}')"
        [ "$hash" = "$EXPECTED_IMG_SHA256" ] \
            || { echo "SHA-256 mismatch: got $hash (want $EXPECTED_IMG_SHA256)" >&2; exit 1; }
        note OK
        if [ -f "$IMG.xz" ]; then
            step "Verifying SHA-256 of the xz download"
            hash="$(sha256sum "$IMG.xz" | awk '{print $1}')"
            [ "$hash" = "$EXPECTED_XZ_SHA256" ] \
                || { echo "xz SHA-256 mismatch: got $hash" >&2; exit 1; }
            note OK
        fi
    fi

    raw="$IMG"; gz="$IMG.gz"
    [ -f "$gz" ] || { step "Compressing to $(basename "$gz") (DSU only accepts .gz/.zip)"; gzip -1 -c "$raw" > "$gz"; }
    raw_size=$(stat -c %s "$raw")
    gz_size=$(stat -c %s "$gz")
    free=$(data_free_bytes)
    needed=$(( raw_size + USERDATA_SIZE + gz_size + 1073741824 ))

    step "Sizing"
    note "image:     $raw_size bytes raw, $gz_size bytes gz"
    note "gsi-size:  $raw_size"
    note "userdata:  $USERDATA_SIZE"
    note "data free: $free"
    [ "$free" -ge "$needed" ] || { echo "Not enough space on /data: need $needed, have $free" >&2; exit 1; }

    step "Pushing $(basename "$gz") to /sdcard/system.img.gz"
    adb_ push "$gz" /sdcard/system.img.gz >/dev/null

    step "Starting DSU install (keep the screen unlocked)"
    sh_ am start-activity -n com.android.dynsystem/com.android.dynsystem.VerificationActivity \
        -a android.os.image.action.START_INSTALL \
        -d file:///sdcard/system.img.gz \
        --el KEY_SYSTEM_SIZE "$raw_size" \
        --el KEY_USERDATA_SIZE "$USERDATA_SIZE" >/dev/null

    step "Waiting for the install (up to $TIMEOUT_MIN min); watch the phone notification"
    status="$(wait_for_install)"
    sh_ rm -f /sdcard/system.img.gz >/dev/null || true

    if [ "$status" = "normal" ]; then
        warn "Install did not complete. Recent DSU log lines:"
        adb_ logcat -d -t 800 -s InstallationAsyncTask DsuService DsuAvb gsid DynamicSystemService 2>&1 | tail -30
        echo "Check the DSU notification, then run: $0 cancel" >&2
        exit 1
    fi

    case "$status" in
        *disabled*)
            step "Enabling the DSU for the next boot"
            if [ "$SINGLE_BOOT" -eq 1 ]; then sh_ gsi_tool enable --single-boot >/dev/null
            else sh_ gsi_tool enable >/dev/null; fi
            status="$(gsi_status)" ;;
    esac

    step "DSU is ready (status: $(echo "$status" | paste -sd, -))"
    note "The NEXT REBOOT boots the GSI; your stock system is untouched."
    note "Stay on stock:   $0 disable"
    note "Reclaim space:   $0 wipe   (from stock)"

    if [ "$DO_REBOOT" -eq 1 ]; then sleep 3; adb_ reboot; fi
}

case "$ACTION" in
    status)  cmd_status ;;
    install) cmd_install ;;
    enable)
        resolve_target; step "Enabling the installed GSI for the next boot"
        if [ "$SINGLE_BOOT" -eq 1 ]; then sh_ gsi_tool enable --single-boot >/dev/null
        else sh_ gsi_tool enable >/dev/null; fi
        note "$(gsi_status)" ;;
    reboot)  resolve_target; step "Rebooting"; adb_ reboot ;;
    disable)
        resolve_target; step "Disabling the installed GSI (takes effect on next boot)"
        sh_ gsi_tool disable >/dev/null; note "$(gsi_status)"
        [ "$DO_REBOOT" -eq 1 ] && { step "Rebooting"; adb_ reboot; } || true ;;
    wipe)
        resolve_target; step "Removing the GSI and its userdata"
        sh_ gsi_tool wipe >/dev/null; note "$(gsi_status)" ;;
    cancel)
        resolve_target; step "Cancelling any in-progress install"
        sh_ gsi_tool cancel >/dev/null; note "$(gsi_status)" ;;
    *) echo "usage: $0 {status|install|enable|disable|wipe|cancel|reboot}" >&2; exit 2 ;;
esac
