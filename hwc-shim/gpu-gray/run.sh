#!/usr/bin/env bash
# Push and run the GPU gray benchmark on the phone.
#
#   ./run.sh                              # 20 frames, verify against the CPU converter
#   ./run.sh --iters 50 --w 1440 --h 720
#   ./run.sh --src-cached                 # source allocated CPU_*_OFTEN, for comparison
#
# Needs: TD A16 booted, `adb root`, the phone on USB.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
exe="$here/out/gpu_gray_bench"
[ -x "$exe" ] || { echo "build first: ./build.sh" >&2; exit 1; }

adb wait-for-device
# adb root is a no-op if already root; it drops the connection when it restarts adbd
state=$(adb shell id -u 2>/dev/null | tr -d '\r')
if [ "$state" != "0" ]; then
  adb root >/dev/null 2>&1 || true
  adb wait-for-device
fi

adb push "$exe" /data/local/tmp/gpu_gray_bench >/dev/null
adb shell chmod 755 /data/local/tmp/gpu_gray_bench
echo "--- device: $(adb shell getprop ro.build.fingerprint | tr -d '\r')"
adb shell /data/local/tmp/gpu_gray_bench "$@"
