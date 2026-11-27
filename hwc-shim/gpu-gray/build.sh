#!/usr/bin/env bash
# Build gpu_gray_bench for the phone (arm64, no libc++, no libc++_shared).
#
#   ./build.sh                 # uses $ANDROID_NDK_HOME, ~/Android/Sdk/ndk/*, /opt/android-ndk*
#   NDK=/path/to/ndk ./build.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
api=${API:-30}

ndk=${NDK:-${ANDROID_NDK_HOME:-}}
if [ -z "$ndk" ]; then
  for c in "$HOME"/Android/Sdk/ndk/* /opt/android-ndk* /usr/lib/android-ndk; do
    [ -d "$c" ] && ndk=$c
  done
fi
if [ -z "$ndk" ] || [ ! -d "$ndk" ]; then
  echo "NDK not found; set ANDROID_NDK_HOME or NDK" >&2
  exit 1
fi
# newest NDK in the list
ndk=$(ls -d "$ndk" | sort -V | tail -1)

bin=$(ls -d "$ndk"/toolchains/llvm/prebuilt/*/bin | head -1)
cxx="$bin/aarch64-linux-android$api-clang++"
[ -x "$cxx" ] || cxx="$bin/clang++"
[ -x "$cxx" ] || { echo "clang++ not found under $bin" >&2; exit 1; }

mkdir -p "$here/out"
so="$here/out/gpu_gray_bench"

echo "NDK : $ndk"
echo "CXX : $cxx"
echo "OUT : $so"

"$cxx" --target=aarch64-linux-android$api -O2 -g0 \
  -fno-exceptions -fno-rtti -nostdlib++ \
  -Wall -Wextra -Wno-unused-parameter \
  -o "$so" "$here/gpu_gray_bench.cpp" \
  -lEGL -lGLESv2 -landroid -llog -lm

"${bin}/llvm-readelf" -d "$so" | grep NEEDED || true
ls -l "$so"
