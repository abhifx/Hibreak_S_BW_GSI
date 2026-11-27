# Status — 2026-11-27

## Visible 60 fps accepted

Production configuration:

```powershell
pwsh -File hwc-shim/deploy.ps1 `
  -Fast 0 -Xproc 1 -XprocVendor 1 -HelperOnebit 0 -Mode -1 `
  -Nofence 0 -EinkVsync 1 -Log 1 -LogEvery 60 -Watchdog 90
```

| test | physical vendor commits | rate | fallbacks |
|---|---:|---:|---:|
| Hyper 60 s final soak | 3593 | 59.88/s | 0 |
| Post-recovery Hyper 10 s | 602 | 60.2/s | 0 |
| Post-reboot Hyper 10 s | 598 | 59.8/s | 0 |
| Final clean-boot Hyper 10 s | 600 | 60.0/s | 0 |

The 60 s soak held `SUnreclaim` near 281 MB with stable composer/helper RSS and
bounded fd/dma-buf counts. Helper recovery completed in 4 seconds without a
composer restart. HD 256/Regal bypass the helper and retain the vendor pipeline.
SELinux remains enforcing.

## Architecture

- HWC sends the real RGBA client target to the helper (`GB01`).
- GLES2/PowerVR converts it to packed gray.
- The helper writes into the vendor's own gray commit dma-buf (`HWO1`).
- The vendor worker performs waveform selection, physical commit, fences,
  scan-out, input handling, and ghost cleanup.

Four imported client targets are cached by dma-buf identity. The persisted profile
sets `debug.sf.luma_sampling=0`, removing unnecessary RegionSampling passes.
Average helper conversion is about 11–12 ms.

## Invalid path

Do **not** use `-Fast 1 -Xproc 1`. It commits the helper's separate buffer and can
leave the physical panel frozen. Use `-XprocVendor 1`.

## E-ink Center and sleep

E-ink Center 0.3.5 adds per-app modes, extra-button actions, lock-wallpaper
cycling, dual frontlight control, and an optional one-shot sleep wallpaper. The
sleep frame appears before timeout or physical power-button sleep, is cancelled
by new input, and is skipped for screen-bright wake locks. AOD remains off and the
device enters normal deep doze.

Touch/double-tap wake is unsupported by this kernel/driver; use the power button.

## Persistence and rollback

`hwc-shim/deploy.ps1` installs an idempotent boot re-apply script. E-ink Center
launches it after DSU boot, restoring the bind mount, cached helper, watchdog,
and saved profile. `deploy.ps1 -Revert` writes `persist.disabled`; the overlay then
stays detached across reboots. No boot/kernel/vendor partition or DSU userdata is
modified.
