#!/usr/bin/env bash
# Builds su-stub.apk: a minimal responder for phh-su.
#
# phh's su asks a Superuser app (hardcoded as me.phh.superuser, Koush
# protocol) for a decision over a unix socket and accepts exactly "ALLOW" or
# "DENY". This package answers for the E-ink Center only and denies the rest.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BT=/opt/android-build/android-14
PLATFORM=/opt/android-build/android-35/android.jar
OUT="$HERE/stub/build"

rm -rf "$OUT"; mkdir -p "$OUT/classes" "$OUT/dex"
javac -source 8 -target 8 -nowarn -classpath "$PLATFORM" -d "$OUT/classes" \
    $(find "$HERE/stub/src" -name '*.java')
"$BT/d8" --lib "$PLATFORM" --min-api 30 --output "$OUT/dex" \
    $(find "$OUT/classes" -name '*.class')
"$BT/aapt2" link -o "$OUT/base.apk" -I "$PLATFORM" \
    --manifest "$HERE/stub/AndroidManifest.xml" \
    --min-sdk-version 30 --target-sdk-version 34
( cd "$OUT/dex" && zip -q "$OUT/base.apk" classes.dex )
"$BT/zipalign" -f 4 "$OUT/base.apk" "$OUT/aligned.apk"
"$BT/apksigner" sign --ks "$HERE/debug.keystore" --ks-pass pass:android \
    --key-pass pass:android --out "$HERE/su-stub.apk" "$OUT/aligned.apk"
ls -l "$HERE/su-stub.apk"
