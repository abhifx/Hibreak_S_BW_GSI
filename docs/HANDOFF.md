# HANDOFF — HiBreak MT6765 / TD Android 16 GSI

Last updated: **2026-11-27**

## Result

Visible 60 fps is accepted with the reversible `xproc_vendor=1` HWC/helper
overlay. Final clean-boot Hyper measurement: **600 commits / 10 s (60.0/s)**,
zero fallbacks, quarantines, crashes, or ANRs.

Deployed source hashes:

- HWC shim: `6e9feea083cee84ee17218886fdfccdf69e089ee440c4019cd920db917aff3d8`
- GPU helper: `f30590ad7da965e714e8ae2ce694cf42190f3d41a7bbc901bc674324113a50cb`
- E-ink Center 0.3.5 APK: `766a3d5c0659e9d8d639a4a16e6d6a03aa111cd405f6cb1974e4542b31a18bf9`

## Current device state

- TD Android 16 GSI runs as a persistent DSU.
- The HWC/helper overlay is mounted and re-applied automatically after boot.
- Original SurfaceFlinger remains installed.
- Katapult is the only Home app; Launcher3 remains only as the hidden
  Recents/quickstep provider.
- Battery-first mode: AOD off, normal doze enabled, 120-second screen timeout.
- `debug.sf.luma_sampling=0` and `xproc_damage=0` are persisted.
- E-ink Center's sleep wallpaper is enabled. It shows the next image from
  `Pictures/EinkWallpapers` before timeout or physical power-button sleep, then
  removes itself on wake/new input.
- Touch/double-tap wake is not supported by the kernel/driver; use the power
  button.

## HWC/helper details

Use:

```powershell
pwsh -File hwc-shim/deploy.ps1 `
  -Fast 0 -Xproc 1 -XprocVendor 1 -HelperOnebit 0 -Mode -1 `
  -Nofence 0 -EinkVsync 1 -Log 1 -LogEvery 60 -Watchdog 90
```

The helper caches four imported client targets by dma-buf identity. The 60 s Hyper
soak reached 3593 commits (59.88/s) with stable RSS/PIDs, flat `SUnreclaim` near
281 MB, and bounded fds/dma-bufs. Helper kill/recovery passed in 4 seconds without
restarting the composer. HD 256/Regal bypass the helper.

The older direct mode `-Fast 1 -Xproc 1` is invalid and can freeze the physical
panel. Do not use it.

## Rejected paths

- HBPC SurfaceFlinger binary swap: display AIDL mismatch.
- Custom baked `system-td-eink.img`: unreliable DSU/FEC/AVB path; deleted.
- Custom kernel driver or waveform: locked bootloader and proprietary waveform.
- Downloaded Lineage GSIs and the Lineage build distro: removed during cleanup.
  The repeatable recipe and small patch archive remain in the full local project,
  not in this public repository.

## Public repository layout

| path | purpose |
|---|---|
| `eink-center/` | app source, build scripts and root-stub source |
| `einkui-shade/` | SystemUI shade plugin source and build scripts |
| `hwc-shim/` | HWC interposer, GPU helper, device scripts and findings |
| `tools/` | DSU, measurement, accessibility and recovery helpers |
| `docs/` | status, research notes, GSI details and compatibility audit |

Release assets are attached to `v1.1.0`. No paid third-party applications are
included.

## Rollback

```powershell
pwsh -File hwc-shim/deploy.ps1 -Revert
```

This writes `persist.disabled`; the overlay stays detached after reboot. A normal
deploy removes the marker. Do not wipe the DSU or flash boot/vendor/vbmeta/kernel.
