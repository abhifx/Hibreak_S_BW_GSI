# HiBreak E-ink SystemUI shade

A HiBreak-safe variant of the B1051C SystemUI overlay plugin. It is a normal
platform-signed APK loaded by SystemUI on debuggable/test-key GSI builds.

It provides:

- Light, opaque black-and-white notification/Quick Settings shade.
- Active QS tiles filled black with white icons; inactive tiles outlined.
- Outlined notification cards and monochrome header/footer treatment.
- Dual cold/warm LM3630A frontlight sliders in Quick Settings.
- Frontlight changes broadcast to `com.hibreak.einkcenter.SET_LIGHT`, where
  E-ink Center's root shell writes the hardware nodes.

The package remains `org.b1051c.einkui` for compatibility with the original
plugin's settings keys and SystemUI plugin interface.

## Install

```sh
adb install -r einkui.apk
adb shell settings put secure b1051c_shade_theme 1
adb shell settings put secure b1051c_shade_palette 0
adb shell settings put secure b1051c_lock_style 0
adb shell settings put secure b1051c_fl_cold 0
adb shell settings put secure b1051c_fl_warm 0
adb shell pkill -f com.android.systemui
```

`b1051c_shade_theme`: `1` light, `2` dark, `0` follow system, `-1` off.
`b1051c_shade_palette`: `0` plain black/white, `1` the original coloured
InKognito palette.

To revert, set `b1051c_shade_theme=-1` and restart SystemUI, or uninstall
`org.b1051c.einkui`.

## Build on Windows

Place the public AOSP test `platform.pk8` and matching `platform.x509.pem` in
`keys/`, then run:

```powershell
pwsh -NoProfile -File build-win.ps1 -VersionCode 100 -VersionName 1.0-hibreak
```

Output: `build/einkui.apk`. The APK must be signed with the same platform
certificate as the GSI SystemUI to receive `com.android.systemui.permission.PLUGIN`.
