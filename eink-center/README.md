# E-ink Center for the HiBreak (GSI)

Replacement for Bigme's stock "E-ink Center", which lives inside their patched
framework and cannot run on a GSI. Everything it drove is reachable underneath,
and this app exposes it.

Two APKs:

| file | package | purpose |
|---|---|---|
| `eink-center.apk` | `com.hibreak.einkcenter` | the UI |
| `su-stub.apk` | `me.phh.superuser` | answers phh-su requests, needed for root |

## What it controls

| control | mechanism |
|---|---|
| refresh mode | `vendor.xrz.global_refresh_mode` (global, live) + `eink_debug/global_mode`; stock names: HD 256 = 2147483825, Fast 178, Extreme 179, Regal 180, Hyper 183, A2 16 |
| panel fix | `persist.vendor.xrz.fps{,_high,_low}` + `service call SurfaceFlinger 1008 i32 1` |
| full refresh | screen off/on cycle (e-ink does a GC refresh on wake) |
| dither | `vendor.xrz.gray_256_dither` + `eink_debug/global_dither` |
| contrast / brightness / saturation | `eink_debug/{contrast,brightness,saturation}` |
| anti-flicker | `vendor.xrz.anti_flicker_enabled` + `eink_debug/anti_flicker` |
| auto clean / home clean / frequency | `vendor.xrz.{auto_clean_enabled,enable_home_clean,clean_frequency}` |
| frontlight | `/sys/class/leds/lcd-backlight/brightness` (0-255) |
| animation mode | `vendor.xrz.eink_animation_mode` |
| composer fps | `persist.vendor.xrz.fps{,_high,_low}` (editable: 120/38/30 by default) |
| switch to stock | `gsi_tool disable` + reboot |
| status bar | Quick Settings tile (`EinkTile`): tap toggles fast b1 / quality b2 |

Mode values are passed through unchanged; the composer logs the mode it chose in
`logcat -s hwcomposer` (`[DEV] [normal commit]: mode=b1,...`). Stock commits use
`b1`; the GSI defaults to `b2`.

### Why the panel needs a fix at all

On this GSI the vendor composer never hands frames to the e-ink engine unless
both are true:

* `persist.vendor.xrz.fps{,_high,_low}` are set (Bigme's stock framework
  normally sets them), **and**
* HWC overlays are disabled (`SurfaceFlinger` code `1008`).

Without either, SurfaceFlinger reports the display `ON` and `screencap` shows a
correct image, but the physical panel keeps the boot logo. `BootReceiver`
schedules four alarms (6/12/25/45 s after `BOOT_COMPLETED`), so the panel comes up on
its own.

## Root

The GSI ships phh's `su`, which is `userdebug`-permissive for `shell` but asks a
Superuser app for a decision for ordinary apps. That app (`me.phh.superuser`,
Koush protocol) is not installed and is not in phh's release assets.

`su-stub.apk` fills that hole:

* phh-su starts `me.phh.superuser/com.koushikdutta.superuser.RequestActivity`
  with an extra `socket`;
* the stub connects to that unix socket, reads the flattened request, compares
  `from.uid` with the UID of `com.hibreak.einkcenter` (resolved via
  `PackageManager`, so reinstalls work), and answers `ALLOW` or `DENY`.

Consequences:

* `me.phh.superuser` must stay installed, otherwise the E-ink Center silently
  loses root (every action shows `Permission denied`).
* If the real phh Superuser app is installed later it replaces this stub and
  the grant has to be approved there instead.
* Only the E-ink Center's UID is allowed; other apps are denied.

## Build

Toolchain used here (no Android SDK needed):

```sh
pacman -S jdk17-openjdk
# aapt2/d8/apksigner/zipalign + android.jar
curl -O https://dl.google.com/android/repository/build-tools_r34-linux.zip
curl -O https://dl.google.com/android/repository/platform-35_r02.zip
```

```sh
./build.sh        # -> eink-center.apk
./build-stub.sh   # -> su-stub.apk
```

### Quick Settings tile

Add it once from the shade: pull down twice, tap the pencil, drag **E-ink** into
the active tiles. Tapping it toggles `b1`/`b2`; it works with the app closed
because the su-stub answers the request.

### Frontlight (dual channel)

The panel's frontlight is an **LM3630A** with independent channels, exposed as

```
/sys/devices/platform/11009000.i2c2/i2c-2/2-0036/lm3630a_cold_light   (0-255)
/sys/devices/platform/11009000.i2c2/i2c-2/2-0036/lm3630a_warm_light   (0-255)
```

(the app writes them through a `*` glob so the same build works on the Pro).
`/sys/class/leds/lcd-backlight` is the Android brightness channel and is
independent. The app exposes Level / Cold / Warm.

### Mode speeds measured here (10 s, full-width benchmark)

| mode | commits/s |
|---|---|
| Fast 178 | 6-7 |
| Extreme 179 | 10 |
| Hyper 183 | 24-28 |
| Regal 180 / HD 256 / A2 / GC4 / DU | ~6 |

`debug.sf.enable_{gl,hwc}_backpressure=0` plus
`persist.sys.phh.enable_sf_{gl,hwc}_backpressure=false` adds a few fps.

The kernel's Hyper gate is `panel_width * panel_height < 2,123,521`
(`is_support_hyper_mode`): our 1440x720 panel passes, the 2496x1944 Pro panel
does not — that is why Hyper froze the Pro. `/proc/device-tree/eink` on this
unit reports `eink_max_fps = 0x0f` (15) and `eink_fresh_hz = 0x55` (85).

### Known gap: auto-clean cadence

`vendor.xrz.clean_frequency` only counts frames while
`vendor.xrz.check_commit != 0`, and stock's PowerManager raises it for 800 ms
after each touch/key. Nothing pulses it here yet, so the "N full refreshes"
counter stays idle; the *Full refresh* button works regardless.

### Returning from stock

Stock has no root and the DSU enable call needs a privileged UID, so the only
way back into the GSI is adb:

```sh
adb shell gsi_tool enable && adb reboot
```

`debug.keystore` is checked in on purpose: both APKs must keep the same
signature between updates (`adb install -r` refuses otherwise, and a changed
UID would need a new grant).

## Install

```sh
adb install -r su-stub.apk
adb install -r eink-center.apk
adb shell am start -n com.hibreak.einkcenter/.MainActivity
```

The stub has no launcher icon and no UI; it only answers su requests.
