# HiBreak S BW — Android 16 GSI, e-ink refresh work

This repo is the working set from getting a **Bigme HiBreak S (black/white, MediaTek
MT6765, 720x1440 mono e-ink)** running a **TrebleDroid Android 16 GSI as a DSU**, and
everything measured while trying to raise its refresh rate.

**The headline: full-panel refresh is ~28–30 fps on this GSI, and that is a
structural ceiling of the prebuilt SurfaceFlinger + vendor composer pair.**
Getting to 60 fps requires a patched/built system image (SurfaceFlinger e-ink render
pass), not a userspace HAL shim. The exact reasoning, measurements and the concrete
build path are in `docs/` — this repo exists so someone can pick that up.

---

## Device and constraints

| | |
|---|---|
| Device | Bigme HiBreak S, black/white e-ink, 720x1440, MT6765 |
| Android | TrebleDroid `treble_arm64_bvS` Android 16 (`BP2A.250605.031.A3`), userdebug |
| Install | GSI as a **DSU** — only the system image is replaceable |
| Bootloader | **locked** — no boot/kernel/vendor flashing |
| Vendor display stack | Bigme `xrz` composer `hwcomposer.mt6765.so` (proprietary) |
| Root | `adb root` works on the GSI; stock A14 refuses |

Nothing here requires unlocking the bootloader. The shim is bind-mounted at runtime
and `adb reboot` restores the stock vendor composer.

## What works today

1. **E-ink stack initialisation (biggest quality win).** The GSI never sets the stock
   properties, and `eink_debug/clean_a2` stays 0, so the kernel's whole-panel fast
   path and ghost cleanup are off — fast modes ghost badly and Regal stutters.
   Setting
   `vendor.xrz.auto_clean_enabled/anti_flicker_enabled/hwc_idle = 1` and
   `clean_a2 = 1`, `anti_flicker = 1` fixes it. `eink-center/` applies this at boot
   (verified after reboot).
2. **HWC shim, converter (`-Fastconv 1`).** Replaces the vendor's RGBA→16-level-grey
   converter with a NEON one: 2.3 ms vs the vendor's ~35 ms of per-frame CPU, output
   verified against the vendor's at 0.35/255 mean difference. Same mode, same
   waveform, `format=0`. Cuts composer CPU; does **not** raise the frame rate.
3. **1-bit dithering (`dither1`, on by default).** A2/Hyper/Extreme are 1-bit and the
   kernel does not dither them, so mid-greys become partial drives (residue). The
   converter dithers to pure black/white with an 8×8 Bayer in those modes.
4. **Stock e-ink properties** applied by `deploy.ps1` (they are `ro.vendor`/`vendor`
   values the stock framework would set).

Measured: vendor path 34–37 ms/commit (~28 fps) at full panel, correct image;
static screen idles (2 commits / 6 s).

## Why not 60 fps with the shim

- The GSI's SurfaceFlinger hands the composer **4 MB of RGBA** every frame.
- The vendor pipeline copies those 4 MB into a staging buffer — **31–80 ms, avg
  33.5 ms** — and that copy is the input to its converter, so it cannot be skipped
  from outside (`skipcopy` produced 63 commits/s of **black** frames).
- Raw `mmap` of the client target dma-buf is **uncached** (~13 MB/s, 80 ms); ION
  import is rejected (`ENOTTY`); locking an imported gralloc handle from inside the
  composer **crashes** it (`SIGSEGV` in `gralloc.rogue.so`, tombstone `Dispatcher_0`).
- Stock and the HiBreak Pro reach their speeds because their **SurfaceFlinger
  pre-converts the frame** to 1-bit/16-level grey, so the composer moves ~1 MB.
  That is a system-image change.

Details: `docs/STATUS.md`, `docs/RESEARCH-NOTES.md`, `hwc-shim/FINDING-fast-commit.md`.

## Quick start

Prerequisites: the TD A16 GSI running as a DSU, `adb root` working, Android SDK
build-tools + a JDK for the APK, NDK for the shim. Windows commands below; the
scripts are PowerShell.

```powershell
# 1. e-ink quality fix (persistent): build + install E-ink Center once
pwsh -File eink-center/build-win.ps1
adb install -r eink-center/eink-center.apk
adb reboot        # it applies clean_a2/anti_flicker at boot

# 2. the HWC shim (runtime, reversible): converter only
pwsh -File hwc-shim/build.ps1
pwsh -File hwc-shim/deploy.ps1 -Fastconv 1 -Log 1 -LogEvery 40

# 3. measure
adb push hwc-shim/device/measure.sh /data/local/tmp/
adb shell 'sh /data/local/tmp/measure.sh 8'

# revert everything
adb reboot        # or: pwsh -File hwc-shim/deploy.ps1 -Revert
```

No vendor binaries are included (the shim `dlopen`s the device's own
`/vendor/lib64/hw/hwcomposer.mt6765.so`). The `.so` in this repo is your build.

## How to help — the 60 fps path

The missing piece is **SurfaceFlinger converting the frame for the panel** (and
passing client-target damage), exactly as the HiBreak Pro LineageOS project does:

- https://github.com/eckelhesten/BigmeHibreakPro-LineageOS

Their published patches that matter:

| patch | why |
|---|---|
| `frameworks/native/0013-sf-e-ink-render-pass...` | converts the composed frame to what the panel shows (1-bit / 16-level grey) |
| `frameworks/native/0004` / `0002` (damage) | tells the composer which part of the client target changed; without it the vendor copies/commits the full panel |
| `frameworks/native/0006` (motion fast) | A2 while the screen moves |

Two routes, both without unlocking the bootloader:

1. **Build a system image** from the same Android 16 branch with those patches, adapted
   to 720x1440, install it as a DSU. This is the only route that can reach 60 fps.
   See `docs/BUILD-GSI.md`.
2. **Build just `surfaceflinger`** from the same branch with the patches and bind-mount
   it over `/system/bin/surfaceflinger` (smaller deployment, higher ABI risk).
   `tools/deploy-pro-sf.sh` can deploy a binary once you have one; `tools/extract-pro-sf.sh`
   extracts the Pro's patched binary from their release for a quick compatibility test.

Open questions that decide the design:

- Does the vendor composer copy only the damaged region when SF sends client-target
  damage? Measured behaviour today: no CT damage → full 4 MB copy every frame.
- Which 8-bit client-target path does the driver accept without the 4 MB copy?
- Is there a safe, cached CPU mapping of the client target in the composer process
  (mapper import+lock crashes; is there another API/format/usage that does not)?

## Repo layout

```
hwc-shim/        HWC2 interposer for hwcomposer.mt6765.so (source, deploy, device scripts)
eink-center/     the app that applies the e-ink fix at boot, plus its build scripts
tools/           USB-adb fixes, Pro-SF extraction/deploy, DSU helpers
docs/            STATUS, ROADMAP, BUILD-GSI, HARDWARE, RESEARCH-NOTES, HANDOFF
```

## Safety

- Runtime-only changes (bind mounts, tmpfs, properties). `adb reboot` reverts.
- Do not flash this device: locked bootloader, DSU only.
- The vendor composer is proprietary and not redistributed here.

## Credits

- HiBreak Pro LineageOS patches and the stock `xrz` property/kernel-knob knowledge:
  https://github.com/eckelhesten/BigmeHibreakPro-LineageOS
- B1051C shim for the GOT-hook technique and the `commit_use_ct` idea.
