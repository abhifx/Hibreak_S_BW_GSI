#!/usr/bin/env bash
# Build gray_helper for Android arm64 (API 30, no libc++/libc++_shared).
#
#   ./build.sh
#   NDK=/path/to/android-ndk API=30 ./build.sh
#
# NDK discovery: $ANDROID_NDK_HOME, $ANDROID_NDK_ROOT, then common SDK/NDK paths.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
api=${API:-30}
ndk=${NDK:-${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}}

if [ -z "$ndk" ]; then
    for candidate in "$HOME"/Android/Sdk/ndk/* /opt/android-ndk* /usr/lib/android-ndk; do
        if [ -d "$candidate" ]; then ndk=$candidate; fi
    done
fi
if [ -z "$ndk" ] || [ ! -d "$ndk" ]; then
    echo "NDK not found; set ANDROID_NDK_HOME or NDK" >&2
    exit 1
fi

# Permit NDK to name either the toolchain root or a directory containing it.
if [ ! -d "$ndk/toolchains/llvm/prebuilt" ]; then
    for candidate in "$ndk"/*; do
        if [ -d "$candidate/toolchains/llvm/prebuilt" ]; then ndk=$candidate; fi
    done
fi

bin=
for candidate in "$ndk"/toolchains/llvm/prebuilt/*/bin; do
    if [ -d "$candidate" ]; then bin=$candidate; fi
done
if [ -z "$bin" ]; then
    echo "LLVM toolchain not found under $ndk" >&2
    exit 1
fi

cxx="$bin/aarch64-linux-android$api-clang++"
if [ ! -x "$cxx" ]; then cxx="$bin/clang++"; fi
if [ ! -x "$cxx" ]; then
    echo "clang++ not found under $bin" >&2
    exit 1
fi

mkdir -p "$here/out"
exe="$here/out/gray_helper"

echo "NDK : $ndk"
echo "CXX : $cxx"
echo "OUT : $exe"

"$cxx" --target=aarch64-linux-android$api -std=c++17 -O2 -g0 -DNDEBUG \
    -fPIE -fno-exceptions -fno-rtti -nostdlib++ \
    -ffunction-sections -fdata-sections -Wl,--gc-sections -Wl,--build-id=sha1 \
    -Wall -Wextra -Wpedantic -Wno-unused-parameter \
    -o "$exe" "$here/gray_helper.cpp" \
    -lEGL -lGLESv2 -lnativewindow -landroid -llog

"$bin/llvm-readelf" -h "$exe" | grep -E 'Class:|Machine:|Type:'
"$bin/llvm-readelf" -d "$exe" | grep NEEDED || true
if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$exe"
elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$exe"
fi
ls -l "$exe"
