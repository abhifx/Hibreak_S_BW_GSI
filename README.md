# HiBreak S BW — Android 16 GSI e-ink work

Working set for the **Bigme HiBreak (original MT6765, 720×1440 mono e-ink)** running
a **TrebleDroid Android 16 GSI as a DSU**.

**Visible 60 fps is achieved with a reversible HWC2 + GPU helper overlay.** The
production configuration keeps the vendor's own gray commit buffer and physical
commit pipeline:

```powershell
pwsh -File hwc-shim/deploy.ps1 `
  -Fast 0 -Xproc 1 -XprocVendor 1 -HelperOnebit 0 -Mode -1 `
  -Nofence 0 -EinkVsync 1 -Log 1 -LogEvery 60 -Watchdog 90
```

Final post-reboot Hyper measurement: **600 physical vendor commits in 10 seconds
(60.0/s)**, zero fallbacks, quarantines, crashes, or ANRs. SELinux remains
enforcing. See [`docs/STATUS.md`](docs/STATUS.md) and
[`hwc-shim/FINDING-xproc-gpu.md`](hwc-shim/FINDING-xproc-gpu.md).

## Release

Download `v1.1.0` from
[GitHub Releases](https://github.com/abhifx/Hibreak_S_BW_GSI/releases/tag/v1.1.0):

- `system-td-arm64-vanilla.img.gz` — verified vanilla TD Android 16 GSI
- `eink-center.apk` — E-ink Center 0.3.5
- `su-stub.apk` — root grant stub for E-ink Center
- `einkui.apk` — light two-ink SystemUI shade plugin
- `SHA256SUMS.txt`

No paid third-party applications are included.

## What works

- **HWC/helper vendor pipeline:** GLES2 converts the real RGBA client target and
  writes packed gray into the vendor's own dma-buf. The vendor worker retains
  waveform selection, fences, scan-out, touch handling, and ghost cleanup.
- **E-ink Center:** refresh modes, panel initialization, full refresh, dither,
  contrast/brightness/saturation, anti-flicker/auto-clean, dual LM3630A frontlight,
  Quick Settings, per-app modes, extra-button actions, lock-wallpaper cycling, and
  an optional one-shot sleep wallpaper.
- **Battery-first sleep:** AOD stays off and normal deep doze remains enabled. The
  sleep wallpaper is shown before timeout or physical power-button sleep and is
  removed on wake or new input. Apps holding a screen-bright wake lock are skipped.
- **SystemUI shade:** light black/white Quick Settings, inverted active tiles,
  outlined notifications, working cold/warm sliders, and Color inversion.
- **Boot persistence:** the runtime bind mount, helper, watchdog, saved profile,
  E-ink Center preset, and wallpaper preference return automatically after reboot.

Touch/double-tap wake is not supported by this kernel/driver; use the power button.

## Quick start

Prerequisites: TD Android 16 GSI running as a DSU, `adb root`, Android SDK
build-tools + JDK for the APKs, and NDK for the HWC/helper build. The APKs and GSI
are release assets; source archives contain the source and build scripts.

```powershell
# 1. Install the app and root stub from the release assets.
adb install -r eink-center/su-stub.apk
adb install -r eink-center/eink-center.apk
adb install -r einkui-shade/einkui.apk

# 2. Enable E-ink Center's accessibility service.
adb push tools/enable-eink-accessibility.sh /data/local/tmp/
adb shell sh /data/local/tmp/enable-eink-accessibility.sh

# 3. Build and deploy the reversible visible-60-fps overlay.
pwsh -File hwc-shim/build-all.ps1
pwsh -File hwc-shim/deploy.ps1 `
  -Fast 0 -Xproc 1 -XprocVendor 1 -HelperOnebit 0 -Mode -1 `
  -Nofence 0 -EinkVsync 1 -Log 1 -LogEvery 60 -Watchdog 90

# 4. Measure physical vendor commits.
adb push tools/measure-xproc-vendor.sh /data/local/tmp/
adb shell sh /data/local/tmp/measure-xproc-vendor.sh 10 183 1
```

Revert the HWC overlay with `hwc-shim/deploy.ps1 -Revert` or reboot. The boot
re-apply script honors the revert marker.

## GSI

The release asset is the untouched vanilla TD image:

- raw SHA-256: `a0704a7909328cc077f314f25ed47c5ce02b4a095cc932dd36060e4a90d333a8`
- gzip SHA-256: `2df4b1a0a005195cba5e0bf271543f1e69ceb6b991fb8585a222e80875bed266`

See [`docs/GSI-IMAGE.md`](docs/GSI-IMAGE.md) for DSU installation. Do not use the
old custom `system-td-eink.img` experiment.

## Repository layout

```text
eink-center/     E-ink Center source, build scripts, root stub source
einkui-shade/    SystemUI shade plugin source and build scripts
hwc-shim/        HWC2 interposer, GPU helper, device scripts, findings
tools/           DSU, measurement, accessibility, recovery and SF helpers
docs/            status, handoff, research notes and GSI build audit
```

## Safety

- Runtime-only changes: bind mounts, tmpfs, properties, and user-space services.
- Do not flash boot, vendor, vbmeta, kernel, or recovery; the bootloader is locked.
- Do not wipe the working DSU.
- Do not use `-Fast 1 -Xproc 1`; the direct helper-buffer commit path is invalid and
  can leave the physical panel frozen. Use `-XprocVendor 1`.
- The vendor composer is proprietary and is not redistributed here.

## Credits

- TrebleDroid and Pierre-Hugues Husson for the GSI/Treble platform.
- HiBreak Pro LineageOS and the B1051C HWC shim for e-ink research and hook ideas.
- A9/HiBreak patching-service projects for input, frontlight, and accessibility
  patterns.
