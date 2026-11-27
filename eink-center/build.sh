#!/usr/bin/env bash
# Build + sign the E-ink Center APK with the standalone build-tools.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BT=/opt/android-build/android-14
PLATFORM=/opt/android-build/android-35/android.jar
OUT="$HERE/build"

[ -x "$BT/aapt2" ] || { echo "missing $BT/aapt2" >&2; exit 1; }
[ -f "$PLATFORM" ] || { echo "missing $PLATFORM" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/dex"

echo "==> javac"
find "$HERE/src" -name '*.java' > "$OUT/sources.txt"
javac -source 8 -target 8 -nowarn -classpath "$PLATFORM" -d "$OUT/classes" @"$OUT/sources.txt" 2>&1 \
    | grep -v 'bootstrap class path' || true

echo "==> d8"
mapfile -t CLASSES < <(find "$OUT/classes" -name '*.class')
"$BT/d8" --lib "$PLATFORM" --min-api 30 --output "$OUT/dex" "${CLASSES[@]}"

echo "==> aapt2 compile resources"
"$BT/aapt2" compile --dir "$HERE/res" -o "$OUT/compiled.zip"

echo "==> aapt2 link"
"$BT/aapt2" link -o "$OUT/base.apk" -I "$PLATFORM" \
    --manifest "$HERE/AndroidManifest.xml" \
    --min-sdk-version 30 --target-sdk-version 34 "$OUT/compiled.zip"

echo "==> add dex"
( cd "$OUT/dex" && zip -q "$OUT/base.apk" classes.dex )

echo "==> zipalign"
"$BT/zipalign" -f 4 "$OUT/base.apk" "$OUT/aligned.apk"

if [ ! -f "$HERE/debug.keystore" ]; then
    echo "==> keystore"
    keytool -genkeypair -keystore "$HERE/debug.keystore" -alias androiddebugkey \
        -storepass android -keypass android -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
fi

echo "==> apksigner"
"$BT/apksigner" sign --ks "$HERE/debug.keystore" --ks-pass pass:android \
    --key-pass pass:android --out "$HERE/eink-center.apk" "$OUT/aligned.apk"

"$BT/apksigner" verify --print-certs "$HERE/eink-center.apk" | head -5
ls -l "$HERE/eink-center.apk"
