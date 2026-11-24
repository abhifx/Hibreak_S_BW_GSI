# HANDOFF — HiBreak MT6765 / TD Android 16 GSI

Last updated: **2026-11-20**  
Working directory: `<repo>`

---

## 1. Mission

Bigme HiBreak (original, MT6765, 5.84" 720x1440 mono e-ink) running a
TrebleDroid **Android 16 GSI** as a DSU.

Current e-ink composer limit on TD:

- every frame commits the **full panel** `(0,0,1439,719)`
- panel IRQ ~**42–45 ms**
- result ~**23–24 fps** for full-screen content
- stock A14 reaches **60–66 fps** on the same panel

The missing piece is **partial / damage-based e-ink updates**, not a custom
waveform (we already have Hyper/A2).

**Goal:** get TD above 24 fps without a full source build. The 2026-11-20
measurement moved the bottleneck off the panel: the vendor's worker burns ~32 ms
of one core per frame re-converting the already-composited client target. The
shim now has a **fast path that owns the commit from `presentDisplay`** and hands
the RGBA client target to the kernel, skipping the vendor worker entirely
(`build/hwc-shim-mt6765/FINDING-fast-commit.md`). Partial rectangles are now a
second-order optimisation on top of that.

---

## 2. What has been proven / ruled out

### Proven

- TD is full-panel / ~24 fps; stock is 60–66 fps.
- The vendor composer already has the concept of an update rectangle
  (`update_area`) and fast modes (`Hyper 183`, `A2 16`, `HD_256 2147483825`).
- The B1051C shim hooks the exact low-level path:
  - `mtk_commit(const drm_eink_update*)`
  - a 68-byte `drm_eink_update` struct with `left, top, right, bottom, mode,
    dither, fd, format, stride, height`
  - ioctl `0xC04464CF` (`DRM_IOWR(0x40 + 0x8f, 0x44)`)
  - client-target fd extraction and picture-diff / page-turn logic
- That makes a userspace partial-update shim plausible without SF source.

### Ruled out

- **HBPC SurfaceFlinger binary swap**: SF runs, but AIDL/display interface
  mismatch makes `system_server` crash (`Timeout waiting for default display`).
- **Custom baked `system-td-eink.img`**: bootloader/DSU path is a dead end.
  - no-FEC re-sign falls to fastboot;
  - FEC re-sign installs unreliably; DSU `VerificationActivity` state broke and
    multiple stock reboots/resets did not restore a reliable install.
  - Keep the image as reference; **do not flash it**.
- **Custom kernel driver / custom waveform**: locked bootloader, no panel
  waveform format/key, high risk of panel damage.
- **ESP32-style custom LUT**: doesn't address the 1440-line full-panel scan.

---

## 3. Current phone / repo state

### Phone

- TD A16 GSI restored from `gsi/system-td-arm64-vanilla.img`; DSU enabled.
- BookLauncher Home, Jelly browser, Readest, KOReader, Lineage app suite.
- E-ink Center with:
  - frontlight off on lock / restore on wake (LM3630A, both channels)
  - lock-wallpaper cycler from `/sdcard/Pictures/EinkWallpapers`
  - AOD/lock-screen fix, QS tiles, light/high-contrast UI, auto-clean.
- `/sdcard` and app data backed up under `sdcard-backup/`.
- Phone may be disconnected; boot TD and enable USB debugging.
- `adb root` works on TD (userdebug); **not** on stock.

### Important files

| path | purpose |
|---|---|
| `gsi/system-td-arm64-vanilla.img` | known-good TD A16 image (do not modify) |
| `gsi/system-td-eink.img` | FEC-signed custom image experiment; **do not flash** |
| `build/eink-center/` | E-ink Center APK/source |
| `build/eink-image/payload/` | 129 MB app payload (E-ink Center, BookLauncher, Jelly, readers, Lineage apps) |
| `build/eink-image/system/eink/setup.sh` | first-boot/restore script |
| `build/eink-provision/provision.sh` | one-command restore from host |
| `refs/b1051c/` | full B1051C reference repo (shim source, docs) |
| `build/hwc-shim/hwcshim-b1051c.cpp` | copied B1051C shim source |
| `build/hwc-shim/b1051c-eink-compat-v0.1.55.zip` | prebuilt module (shim `.so`, scripts) |
| `build/hwc-shim/README.md` | detailed shim plan |
| `sdcard-backup/` | `/sdcard` + app data backup |
| `NOTES.md` | full research log |

---

## 4. HWC shim: what is now known, and what to change

**2026-11-20: the B1051C shim is the right architecture, wrong device.** Our own
library was reverse-engineered; use `build/hwc-shim-mt6765/` (built, not yet run).

Library: `/vendor/lib64/hw/hwcomposer.mt6765.so`, BuildID
`0bc3c35a2ffd85ff39c768deb4b0230b` (copy at `build/hwc-shim-mt6765/real/`).

```
DispDevice::triggerEinkCommit(CommitInfo*)  0xf52a0
  -> mtk_commit(eink_update_config)         PLT call (0xf5738 / 0xf5984)
       -> ioctl(fd, 0x40444FE4, &cfg)       0xef900
```

`0x40444FE4` = `_IOW(0x4f, 0xe4, 68)`. **`eink_update_config` is not
`drm_eink_update`** and the prebuilt module cannot be reused:

| offset | field |
|---|---|
| +0x00 | `u32 mode` (0xb1 HD, 0xb2 Fast, 0xb3 Extreme, 0x11 Hyper, 0x10 A2, 0x800000b1 HD256) |
| +0x04 | `left` (inclusive) |
| +0x08 | `top` |
| +0x0c | `right` |
| +0x10 | `bottom` |
| +0x14 | `dither` |
| +0x18 | `u32[4]` zero |
| +0x28 | `fd` |
| +0x2c | `format` |
| +0x30 | `u32[2]` zero |
| +0x38 | `stride` |
| +0x3c | `height` |
| +0x40 | `1` |

The rect is in the **1440x720 controller buffer**, not the 720x1440 SF display.
`mtk_commit` is a pass-through to the ioctl, and it is called through the PLT, so
its GOT slot is hookable.

Why the GSI is full-panel: the vendor computes the rectangle itself
(`CommitPolicy`), and on the GSI it always degenerates to `(0,0,1439,719)` — one
full 1440-line scan ≈ 42–47 ms ≈ 24 fps. Stock reaches 66 commits/s in Hyper on
the same composer, so a shorter rectangle is the whole gain.

### Plan (`build/hwc-shim-mt6765/README.md`)

**2026-11-23: the bottleneck is the vendor worker's 32 ms of CPU, not the
rectangle.** `FINDING-cpu-bound.md` + `FINDING-fast-commit.md`. The shim grew a
fast path (`fast=1`) that commits from `presentDisplay` and drops the vendor
worker's commits; see section 4b.

1. `pwsh -File build.ps1` — already done, `out/hwcomposer.mt6765.so`.
2. `pwsh -File deploy.ps1 -Disable` — passthrough smoke test (labels/loading).
3. `pwsh -File deploy.ps1 -Rect 0` — **observe only**: per-commit `dt`, vendor
   rect/mode, and the damage rects SF supplied (`ct=` from `setClientTarget`,
   `layer=` from `setLayerSurfaceDamage`). Read `fmt=`, `stride=`, `h=` here:
   they define what the kernel expects.
4. `pwsh -File deploy.ps1 -Fast 1 -VendHz 2` — **the fast path**: RGBA client
   target committed directly, vendor worker throttled. If the panel is wrong,
   `-Fast 1 -Inplace 1` (RGBA→gray8 in place, format 0). `-FastDiff 1` adds the
   pixel diff that makes a static screen commit nothing at all.
5. Only after the fast path is stable does the rectangle matter:
   `-Fast 1 -FastDiff 1` is usually better than the vendor's rect logic.

Revert: `adb reboot` (bind mount + tmpfs only).

### 4b. Status: ~28-30 fps ceiling on a prebuilt GSI (corrected 2026-11-24)

**Correct mode: `-Fastconv 1`** (our converter, 2.3 ms, verified 0.35/255) with
the vendor copy, plus the stock `vendor.xrz.*` properties and
`eink_debug/clean_a2` (both were **0** on the GSI; the deploy/apply sets them).

Why not 60: the GSI's SF hands the composer 4 MB RGBA per frame; the vendor's
4 MB staging copy costs 31-80 ms (avg 33.5 ms) and is the converter's input, so
skipping it blacks the panel (the 63 commits/s "60 fps" run was black frames).
Raw mmap of the client target is uncached (80 ms) and ion_import is rejected
(ENOTTY). Stock/Pro reach 60 only because their SF pre-converts to 1-bit grey
(Pro repo patch 0013) so the composer copies 1 MB.

60 fps therefore needs a patched/built **system image** (SF e-ink render pass),
not a userspace HWC shim. Details: `NOTES.md` 2026-11-24 06:50.

Full rationale, the two open questions and the exact test sequence:
`build/hwc-shim-mt6765/FINDING-fast-commit.md`.

### 4c. The vendor already has the skip (disassembly, 2026-11-23)

`OverlayEngine::convertLayerToGray` (0xa1260) reads `CommitInfo+0x54` as the
pixel format and **skips its in-place `convertToGray` when it is non-zero**
(`cbnz w27` at 0xa1488); `triggerEinkCommit` copies the same field into the
`eink_update_config.format` it passes to the ioctl. Zero = gray8, convert on the
CPU; non-zero = the buffer is RGBA, let the kernel convert. The GSI's job carries
0, which is exactly the 32 ms.

`processImage` is PLT-called (slot 0x1233f8), so `skipconv=1` writes
`CommitInfo+0x54 = 1` in `hook_processImage` and the vendor does everything else
itself. That is the smallest possible fix; try it before `fast=1`.


---

## 5. Next session commands

```bash
cd <repo>   # or D:\AI_Development\Hibreak_Android16

# 1. Ensure TD is running and rooted
adb devices
adb shell getprop ro.build.fingerprint     # must contain treble_arm64_bvS
adb root && adb wait-for-device

# 2. Re-pull the vendor HWC if the sha changed (deploy checks it for you)
adb pull /vendor/lib64/hw/hwcomposer.mt6765.so build/hwc-shim-mt6765/real/

# 3. Build (NDK on this host: %LOCALAPPDATA%\Android\Sdk\ndk\28.2.*)
pwsh -File build/hwc-shim-mt6765/build.ps1

# 4. Passthrough first, then an observe-only run; read the log before rewriting
pwsh -File build/hwc-shim-mt6765/deploy.ps1 -Disable
pwsh -File build/hwc-shim-mt6765/deploy.ps1 -Rect 0 -LogEvery 30
adb shell 'logcat -d -s hwcshim' > build/hwc-shim-mt6765/hwcshim-observe.log

# 5. Then, once xform is known: partial rects
pwsh -File build/hwc-shim-mt6765/deploy.ps1 -Rect 1 -Xform <0..5>

# 6. Correct mode: our converter only (~28-30 fps, correct image)
pwsh -File build/hwc-shim-mt6765/deploy.ps1 -Fastconv 1 -Log 1 -LogEvery 40
#   deploy also sets the stock vendor.xrz.* props; apply.sh writes
#   eink_debug/clean_a2 + anti_flicker (both were 0 on the GSI).
#   skipcopy=1 is NOT a speed mode: it commits black frames.
adb shell 'sh /data/local/tmp/measure.sh 8'      # commits/s + app frames

# Revert everything (bind mounts + tmpfs only)
adb reboot

# Restore apps/settings if the DSU was wiped
bash build/eink-provision/provision.sh
```

If the phone is on stock instead of TD:

```bash
tools/dsu.sh status
tools/dsu.sh enable          # or install the original TD image
adb reboot
```

---

## 6. Risks / cautions

- HWC shim bugs can crash the composer/display. Always test with bind mounts;
  `adb reboot` restores the vendor library.
- Some vendor HWC internals are BuildID-specific in the B1051C source. Our
  BuildID will differ; verify symbols/struct layout before enabling hooks.
- The vendor may ignore a smaller update rectangle or still run a full-panel
  waveform. Benchmark before assuming success.
- Do not flash the custom image; do not wipe the DSU without confirmation.
- No kernel/boot changes are possible on the locked bootloader.
