# E-ink Center for the HiBreak (GSI)

Replacement for Bigme's stock E-ink Center on the TrebleDroid Android 16 GSI.
It is a normal debug-signed user app. Root operations use the companion
`me.phh.superuser` stub, which grants root only to `com.hibreak.einkcenter`.

## Features

| control | mechanism |
|---|---|
| refresh mode | `vendor.xrz.global_refresh_mode` + `eink_debug/global_mode` |
| panel fix | composer FPS properties + SurfaceFlinger code 1008 |
| full refresh | one-shot full-panel GC16 repaint; never changes power/lock state |
| dither | `vendor.xrz.gray_256_dither` + `eink_debug/global_dither` |
| contrast / brightness / saturation | `eink_debug/{contrast,brightness,saturation}` |
| anti-flicker / auto-clean | vendor properties + kernel debugfs nodes |
| frontlight | LM3630A dual-channel cold/warm nodes |
| Quick Settings | light two-ink shade, dual cold/warm sliders, Color inversion, E-ink menu, refresh, frontlight |
| floating menu | optional draggable bubble plus on-demand accessibility-overlay panel |
| navigation | Android gesture navigation; optional Back/Home/Recents overlay, off by default |
| per-app modes | package profiles switched on foreground-app changes |
| extra button | PMIC `KEY_VOLUMEUP` / legacy scan 647 with configurable actions |

Supported modes:

```text
HD 256   2147483825  (0x800000B1)
Regal    180
Fast     178
Extreme  179
Hyper    183
A2       16
```

Mode values are stored as strings because HD 256 exceeds signed Java `int`.

## Floating menu and per-app modes (0.3)

Open **Floating & per-app** near the top of E-ink Center and enable **E-ink
floating & profiles** in Android's accessibility settings. The service uses a
`TYPE_ACCESSIBILITY_OVERLAY`; no System alert-window permission is required.

The bubble is **optional and off by default**. Existing users who explicitly
enabled it keep that choice. The panel does not depend on the bubble:

- Tap the **E-ink menu** Quick Settings tile to open/toggle the panel; hold it
  to open the app settings. It never changes the refresh mode.
- Tap **E-ink refresh** for a one-shot full-panel GC16 repaint. It does not send
  power keys, sleep the display, or lock the device.
- Double-press the extra hardware button (the default `Open menu` action).
- If enabled, tap the black **E** bubble to expand the panel; hold it to open
  the main app, or drag it to move it. Position is saved.
- The panel includes all six modes plus a dedicated **Toggle Fast / Hyper** action.
- **Per-app modes** lists launchable apps. Each app can inherit the global mode
  or choose HD 256, Regal, Fast, Extreme, Hyper, or A2.
- Android exposes one global composer mode, so profiles switch that mode when
  the focused app changes. Split-screen follows the focused application.
- While the notification/Quick Settings shade is open, E-ink Center temporarily
  uses Hyper so the shade paints quickly, then restores the app's mode.
- A bottom-edge touch temporarily uses Hyper for 10 seconds so the transient
  system navigation bar can render on HD 256/Regal apps.
- HWC debug `mode` remains `-1`, allowing the vendor pipeline to choose the
  physical waveform and cleanup behavior.

## Memory/process fixes (2026-11-26)

- All root actions use the existing persistent root shell instead of spawning a
  new `su` process for each property/lighting read or write.
- The daemon reaps empty Android per-PID cgroup2 directories on boot, screen
  transitions and throttled input pulses. Removing 1,011 stale directories freed
  about 976 MB of `SUnreclaim` in the same boot; no live process group is removed.
- Boot and provisioning reclaim empty cgroups and retain the current Home app.
  They no longer automatically disable GMS/GSF/Vending or stop TrebleApp/Launcher3.
- The exact-vendor-destination HWC helper now keeps `mtk_commit`'s fd unchanged,
  uses a persistent socket and cached GPU output resources, and releases ION
  imports/map fds when client-target mappings rotate.

## Navigation

The default is Android **gesture navigation**. E-ink Center sets
`navigation_mode=2` and ensures the gestural navigation overlay is enabled,
including after the GSI's `idmap2d` daemon has timed out. Fullscreen apps use the
standard bottom gesture; no transient three-button bar is involved.

When the accessibility service is connected, E-ink Center draws a compact
transparent 48 px gesture strip with an 8 dp handle. The strip is visual-only and
non-touchable; Android's own gesture monitors continue to own Home, Back, and
Recents input. Overlay ownership is shared across service instances so a
reconnect removes the previous strip before adding the new one; three tested
reconnect cycles retained exactly one window.

An optional 64 dp Back / Home / Recents accessibility bar remains available from
**Navigation buttons: On/Off** in the floating panel, but it is **off by default**
because it covers the system gesture area.

For root provisioning, enable the service without removing other services:

```sh
adb push tools/enable-eink-accessibility.sh /data/local/tmp/
adb shell sh /data/local/tmp/enable-eink-accessibility.sh
```

## Extra hardware button

The extra button has two verified mappings on this GSI:

- **Scan 647 (`KEY_SIDEVU_SONAR`)** on `gpio_keys` / legacy input paths.
- **Volume Up, scan 115** on `mtk-pmic-keys`.

The accessibility service consumes only those extra-button mappings. The real
Volume Up rocker also reports Volume Up/scan 115, but from `gpio_keys`, so it is
left untouched.

Configurable slots:

| screen state | gestures |
|---|---|
| on | single, double, long |
| off | single, double, long |

Actions: Nothing, Open E-ink menu, Full refresh, Toggle Fast/Hyper, Cycle mode,
Home, Back, Recent apps.

Defaults:

```text
screen on:  single = Full refresh, double = Open menu, long = Nothing
screen off: all = Nothing
```

Double press window is 300 ms; long press is 700 ms. External keyboards and
injected virtual keys are not consumed.

## Complete E-ink SystemUI preset

The **SystemUI (e-ink)** card in E-ink Center provides:

- **Apply complete E-ink UI**: forced light theme (`cmd uimode night no` and
  secure `ui_night_mode=0`), animation/transition/animator scales `0`,
  `disable_window_blur=1`, secure `high_text_contrast_enabled=0` plus the
  AOSP-prefixed contrast key `0`, `heads_up_notifications_enabled=0`,
  `smooth_fling_factor=0`, `vendor.xrz.eink_animation_mode=0`, and secure
  `navigation_mode=0` (3-button navigation). High contrast is intentionally off
  because it draws solid boxes behind Quick Settings labels.
- **Restore Android UI defaults**: scales `1`, blur `0`, high contrast `1`,
  heads-up `1`, smooth-fling removed, and vendor animation mode `1`.
  Restore deliberately leaves the current navigation mode unchanged.

BootReceiver reapplies the complete preset at each existing panel-fix tick and
launches the optional `/data/local/tmp/hwc-shim-mt6765/persist.sh` boot re-apply
script when deployment has installed it. All preset commands run off the UI
thread; the HWC script is idempotent and honors `persist.disabled`.

## SystemUI e-ink shade

`EinkUiShade.apk` is a platform-signed SystemUI overlay plugin for this
debuggable/test-key GSI. It forces a light two-ink shade: active QS tiles are
solid black with white icons, inactive tiles are white with black outlines, and
notification cards are opaque with black outlines. The standard brightness
slider is replaced by working cold/warm LM3630A frontlight sliders; changes are
sent to `LightControlReceiver` in E-ink Center and written through its root shell.

The persistent setup uses:

```text
b1051c_shade_theme=1    light
b1051c_shade_palette=0 plain black and white
b1051c_lock_style=0    leave the lock screen style unchanged
```

To revert the shade without uninstalling, set `b1051c_shade_theme=-1` and
restart SystemUI. Uninstalling `org.b1051c.einkui` also removes the plugin.

## Root

The GSI ships phh's `su`. Ordinary apps need a Superuser responder.
`su-stub.apk` implements the Koush protocol and answers `ALLOW` only for the
E-ink Center UID.

Keep the stub installed. If a real Superuser app replaces it, approve the grant
there. Other packages remain denied.

## Build

Windows:

```powershell
pwsh -NoProfile -File build-win.ps1
```

Linux:

```sh
./build.sh
```

Both scripts compile every Java source, compile/link `res/`, create DEX, align,
and sign with the repository `debug.keystore`. Output: `eink-center.apk`.

The accessibility XML is required for key-filter capability; runtime flags alone
are insufficient on Android 16.

## Install

```sh
adb install -r su-stub.apk
adb install -r eink-center.apk
adb shell am start -n com.hibreak.einkcenter/.MainActivity
adb push tools/enable-eink-accessibility.sh /data/local/tmp/
adb shell sh /data/local/tmp/enable-eink-accessibility.sh
```

The stub has no launcher UI. The enable script preserves the current Home app
and any other enabled accessibility services.

## Performance and cleanup

With the reversible HWC/GPU-helper deployment:

```powershell
pwsh -File hwc-shim/deploy.ps1 `
  -Fast 0 -Xproc 1 -XprocVendor 1 -HelperOnebit 0 -Mode -1 `
  -Nofence 0 -EinkVsync 1 -Log 1 -LogEvery 60 -Watchdog 90
```

Measured full-panel physical vendor commits:

| mode | commits/s |
|---|---:|
| Fast 178 | 62-64 |
| Hyper 183, 16-level gray | 63 |

The helper replaces pixel conversion only. The vendor worker still performs the
physical commit, waveform selection, touch handling, and ghost cleanup.
`clean_a2=1` and `anti_flicker=1` remain active. The input-sync daemon pulses
`vendor.xrz.check_commit` on touch so clean-frequency counting works.

### Sleep wallpaper

E-ink Center 0.3.5 adds an optional **Sleep wallpaper** switch under Lock
wallpapers. It uses the next image from `Pictures/EinkWallpapers` and shows it as
a non-touchable accessibility overlay shortly before the screen timeout expires.
The overlay is removed on wake or any new touch/key input.

The countdown uses PowerManagerService's actual last-user-activity time, honors
the configured screen timeout, and skips apps holding a screen-bright wake lock.
A non-consuming root reader for the physical power key triggers the same frame
for manual sleep. The overlay is removed on wake or any new touch/key input.
It does not enable AOD or keep Android awake: the wallpaper remains on the
bistable panel while the device enters normal deep doze. The accessibility
service must be enabled. The feature is off by default in code and currently
enabled on the device.

Touch/double-tap wake is not supported by this GSI's touch kernel/driver; use the
power button to wake.

Battery-first mode is the default boot policy: AOD/doze-always-on is disabled,
normal Android doze stays enabled, and screen timeout is 120 seconds. This lets
the phone enter deep doze instead of holding light doze for an always-on clock.
Touch/double-tap wake is unsupported by the stock touch driver on this GSI; use
the power button to wake. The unsupported wake-gesture settings are disabled and
the Focaltech gesture node is left Off. The HWC/helper persistence path is
unchanged.

Details: `../hwc-shim-mt6765/FINDING-xproc-gpu.md`.

## Frontlight

LM3630A nodes:

```text
/sys/devices/platform/11009000.i2c2/i2c-2/2-0036/lm3630a_cold_light
/sys/devices/platform/11009000.i2c2/i2c-2/2-0036/lm3630a_warm_light
```

The app resolves the nodes through a glob, exposes Level / Cold / Warm, and
keeps Android screen brightness in sync.

## Revert / stock

With HWC persistence enabled, reboot reapplies the runtime bind mount and helper.
To disable HWC persistence and stay on the vendor composer across reboots:

```powershell
pwsh -File build/hwc-shim-mt6765/deploy.ps1 -Revert
```

To boot stock instead:

```sh
adb shell gsi_tool disable && adb reboot
```

Return to the GSI:

```sh
adb shell gsi_tool enable && adb reboot
```

No partition, kernel, waveform, or DSU image is modified by the app.
