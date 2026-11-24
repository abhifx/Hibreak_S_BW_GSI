# HWC shim for `hwcomposer.mt6765.so` — partial e-ink updates on TD A16

A userspace HWC2 interposer for **this** phone (Bigme HiBreak, MT6765, 720x1440
mono). It is the non-source-build route to a faster refresh rate: no kernel, no
boot image, no SurfaceFlinger build, no DSU image edit.

Rebuilt from scratch for `hwcomposer.mt6765.so`; the B1051C (MT6877) shim in
`../hwc-shim/` does **not** apply — see “Why the B1051C shim could not be used”.

```
build.ps1     NDK build  -> out/hwcomposer.mt6765.so   (arm64, liblog/libdl/libc only)
deploy.ps1    push + label + bind-mount + restart composer (+ revert hint)
real/         the vendor library the shim wraps (copy of build/eink/hwcomposer.mt6765.so)
```

---

## What we found in the vendor library (2026-11-20)

Library: `/vendor/lib64/hw/hwcomposer.mt6765.so`
BuildID `0bc3c35a2ffd85ff39c768deb4b0230b`, 1 209 944 bytes.

### The e-ink commit path is exported and PLT-callable

```
DispDevice::triggerEinkCommit(CommitInfo*)            0xf52a0
  -> mtk_commit(eink_update_config)                   bl ...@plt  (0xf5738 / 0xf5984)
       -> ioctl(eink_fd, 0x40444FE4, &cfg)            0xef900
```

`0x40444FE4` decodes as `_IOW(0x4f, 0xe4, 68)` — a **68-byte, write-only** ioctl.
Because `mtk_commit` is called through the PLT (the library is not linked
`-Bsymbolic`), its GOT slot can be rewritten at runtime: that is the hook.

### `eink_update_config` — 68 bytes, and it is **not** the B1051C layout

Recovered from three independent call sites in `triggerEinkCommit` (normal
commit at `0xf5958`, per-rect commit at `0xf5700`, rect loop) and cross-checked
against the `DbgLogger` format strings
`[rect commit]: index:%zu mode=%x,[%3d, %3d, %3d, %3d],dither=%d,fd=%d,format=%d`:

| offset | field | notes |
|---|---|---|
| +0x00 | `u32 mode` | `0xb1` HD, `0xb2` Fast, `0xb3` Extreme, `0x11` Hyper, `0x10` A2, `0x800000b1` HD256 |
| +0x04 | `u32 left` | inclusive |
| +0x08 | `u32 top` | inclusive |
| +0x0c | `u32 right` | inclusive |
| +0x10 | `u32 bottom` | inclusive |
| +0x14 | `u32 dither` | |
| +0x18 | `u32[4]` | zero |
| +0x28 | `s32 fd` | source buffer |
| +0x2c | `u32 format` | |
| +0x30 | `u32[2]` | zero |
| +0x38 | `u32 stride` | |
| +0x3c | `u32 height` | |
| +0x40 | `u32` | constant 1 |

The rectangle is in the **1440x720 e-ink controller buffer**, not the 720x1440
SurfaceFlinger display. Stock's own log line confirms the convention:
`[normal commit]: mode=b1,update_area=(0,0,1439,719),dither=..`.

Other exported things worth knowing (all useful later, none needed yet):
`CommitPolicy::{prepareDirtyRegion,checkLayersAndSetCommitInfo,isWholeLayerDirty}`,
`OverlayEngine::{processImage,convertLayerToGray,triggerEink}`, `mtk_waitVSync`,
`mtk_set_anti_flicker`, `mtk_set_eink_animation`, `isDrmCommitEnable`.

### Why the GSI commits the whole panel every frame

The vendor recomputes the update rectangle itself, from the layers' damage
(`CommitPolicy`). On this GSI it always ends up with the full panel
(`update_area=(0,0,1439,719)`), which is one full 1440-line scan ≈ 42–47 ms,
i.e. the ~24 fps ceiling. On stock the same composer reaches 66 commits/s in
Hyper, so a shorter rectangle (fewer scanned lines) is the whole difference.

The shim therefore **does not trust the vendor's rectangle**: it collects the
damage SurfaceFlinger already hands over in `setClientTarget` /
`setLayerSurfaceDamage` and rewrites the rectangle in `mtk_commit`.

### Why the B1051C shim could not be used

| | B1051C (MT6877) | this phone (MT6765) |
|---|---|---|
| struct | `drm_eink_update`, 68 B, rect first | `eink_update_config`, 68 B, **mode first** |
| ioctl | `0xC04464CF` `_IOWR(0x40+0x8f,0x44)` | `0x40444FE4` `_IOW(0x4f,0xe4,68)` |
| real lib | `hwcomposer.mt6877.so` | `hwcomposer.mt6765.so` |
| purpose | colour tiling fix, full-area commits | partial rects, throughput |

The prebuilt module in `../hwc-shim/` hard-codes `mt6877` and the wrong struct
order, so it cannot be reused even after a string patch.

---

## How the shim works

Two layers, the same design as the B1051C interposer:

1. **IN — HWC2.** The shim is loaded in place of the vendor library, `dlopen`s
   the real one from `/system/xbin/hwcomposer.mt6765.real.so`, and patches
   `getFunction` on the `hwc2_device` it returns. The real device pointer is
   handed back unchanged, so every real entry point still receives the pointer
   it expects.
2. **OUT — GOT.** The real library's GOT slot for
   `_Z10mtk_commit18eink_update_config` is rewritten to `hook_mtk_commit`, which
   rewrites the by-value config (a caller stack copy, so in-place is safe) and
   then calls the real function.

Damage is **accumulated between commits and cleared by the commit that consumes
it**. The vendor commits from the e-ink worker thread and can lag
SurfaceFlinger by a frame; a union over that interval can over-invalidate but
never misses a changed pixel.

### Knobs (`vendor.debug.hwc.shim.*`, read once when the composer starts)

| knob | default | meaning |
|---|---|---|
| `log` | 1 | 0 quiet, 1 normal, 2 verbose (every frame, every layer) |
| `disable` | 0 | 1 = pure passthrough, no hooks at all |
| `rect` | **0** | 0 = observe only, 1 = rewrite the rectangle from damage (vendor path) |
| `mode` | −1 | −1 keeps the vendor mode; otherwise force e.g. `17` (0x11 Hyper) or `16` (0x10 A2) |
| `src` | 1 | 1 = client-target damage, 2 = union of layer damage |
| `xform` | 0 | display→controller mapping: 0 identity, 1 swap x/y, 2 swap+mirror x, 3 swap+mirror y, 4 rot180, 5 rot180+swap |
| `min_pct` | 1 | only rewrite when the new rect is smaller than the vendor rect |
| `log_every` | 60 | one commit summary line per N commits |
| `test_w` / `test_h` | 0 | force a fixed top-left rect (timing probe, 0 = off) |
| `fast` | **0** | 1 = own the commit from `presentDisplay` (see below) |
| **`skipconv`** | **0** | **1 = force `CommitInfo+0x54 != 0`, so the vendor skips its own RGBA→gray conversion** |
| `skipconv_fmt` | 1 | value written there (any non-zero = "the buffer is RGBA") |
| **`fastconv`** | **0** | **1 = replace `xrz::EffectHandler::convertToGray` with the NEON converter** |
| `verify` | 0 | 1 = also run the vendor's converter and log the difference (keeps its output) |
| `conv_dst_pitch` | −1 | destination pitch in bytes for `fastconv` (−1 = stride/4) |
| `skipcopy` | 0 | 1 = drop the vendor's 4 MB staging copy (**works with the stock e-ink props/clean_a2**) |
| `novsync` | 0 | 1 = bypass `mtk_waitVSync` pacing (12.5–17 ms per commit once the copy is gone) |
| `fast_idle` | 1 | no damage → no commit at all |
| `fast_diff` | 0 | 1 = pixel-diff the client target, commit only the changed bbox |
| `fast_hz` | 0 | 0 = unlimited; else maximum fast commits per second |
| `fast_wait_ms` | 40 | acquire-fence timeout before a fast commit |
| `inplace` | 0 | 1 = RGBA→gray8 in place, commit as `format 0` (fallback) |
| `inplace_dither` | 1 | ordered 4×4 dither to 16 levels for `inplace` |
| `vend_hz` | 0 | >0 = stretch the vendor worker to this rate (it is inert in fast mode) |
| `use_ct` | 0 | vendor path only: commit the RGBA client target instead of its gray buffer |
| `ct_format` | −1 | force the `format` field written for the RGBA client target |
| `dither` | −1 | force the `dither` field (−1 = mirror the vendor's last value) |

Live state is also appended to `dumpsys SurfaceFlinger`:

```
adb shell dumpsys SurfaceFlinger | grep -A8 hwcshim
```

---

## Fast path (2026-11-24) — corrected status: ~28–30 fps ceiling

**Use `-Fastconv 1` only.** It replaces the vendor converter with a NEON one
(2.3 ms, verified 0.35/255 against the vendor's) and is safe. The deploy also sets
the stock `vendor.xrz.*` properties, and `apply.sh` writes
`eink_debug/clean_a2` + `anti_flicker` — all of which were off on the GSI.

Why not 60 fps from here:

- the GSI's SurfaceFlinger hands the composer **4 MB of RGBA** per frame;
- the vendor pipeline's 4 MB staging copy costs **31–80 ms (avg 33.5 ms)** and is
  the **input** to the converter — `skipcopy=1` therefore commits black frames
  (63 commits/s of black, which is what the earlier "60 fps" measurement was);
- raw `mmap` of the client target is uncached (80 ms per 4 MB) and `ion_import`
  is rejected (`ENOTTY`); only the vendor's own `GraphicBuffer::lock` mappings are
  cached;
- stock and the Pro reach 60 because their SurfaceFlinger pre-converts to
  1-bit/16-level grey first (Pro repo patch 0013), so the composer copies 1 MB.

So the correct ceiling is **~28–30 fps** with the vendor's own waveform and image.
60 fps requires a patched/built **system image** with the SF e-ink render pass,
not a userspace HWC shim. Do not use `skipcopy` as a speed mode.


The rest of this section is the earlier reasoning; it is kept because the
mechanisms are real and the two rejected modes below are still useful tests on a
different kernel.

`OverlayEngine::convertLayerToGray` skips its in-place `convertToGray` whenever
`CommitInfo+0x54` (the pixel format) is non-zero, and `triggerEinkCommit` puts
that same field into the ioctl. Zero means "gray8, convert here"; non-zero means
"the buffer is RGBA, the kernel converts". The GSI's job carries 0.

**Rejected on this kernel:** `skipconv=1` (RGBA to the kernel) makes
`processImage` 40 µs, but each commit then costs 13 ms and the panel interval
235 ms — the kernel's RGB→gray is worse than userspace. `fastgray=1` (convert in
place on the client target) costs 90 ms because writes into the uncached dma-buf
are slow, and `skipcopy=1` defeats the vendor's change detection. `fast=1` (own
the commit) is still the route to the 12.5–17 ms per-commit figure, but it needs
its own gray buffer rather than the client target.

`fast=1` description (unchanged): in `presentDisplay` the shim:

1. duplicates the client target fd and its acquire fence (`setClientTarget`),
2. waits for the fence (the frame is then stable),
3. derives the update rectangle from the layer damage and, with `fast_diff=1`,
   narrows it to the pixels that actually changed against a shadow copy,
4. builds `eink_update_config` itself and calls `mtk_commit` directly,
5. sets `fast_active`: from then on the vendor worker's `mtk_commit` calls are
   dropped. `vend_hz` also stretches its vsync pacing so it stops burning a core.

Failure is contained: no fd, an unsignalled fence, an ioctl error or the panel
being off all clear `fast_active`, and the vendor's worker — no longer
suppressed — commits again.

**Read `FINDING-fast-commit.md` before the first run.** The one number that
decides which sub-mode to use is the vendor's own `fmt=` in the observe log.

---

## Measured on the device (2026-11-20)

First instrumented run (`rect=0`, observe only), Settings in the foreground:

```
commit #3 dt=35933us mode=0x11 area=(0,0,1439,719) ct=(0,0,-1,-1)/0 layer=(0,0,-1,-1)/0
commit #4 dt=42422us mode=0x11 area=(0,0,1439,719) ...
commit #5 dt=52826us mode=0x11 area=(0,0,1439,719) ...
```

- The vendor commits the **full panel** in **Hyper (`0x11`)** every frame; `dt`
  is 34–60 ms, i.e. the familiar ~20–25 fps.
- **`setClientTarget` damage is always empty** (`ct=(0,0,-1,-1)/0`) — this HWC
  2.1 build never receives usable client-target damage from A16 SurfaceFlinger.
- `setLayerSurfaceDamage` *is* called, but almost always with the whole layer
  (`(0,0,1440,720)`), and sometimes with a genuinely small rect
  (`(6,74,34,102)`), which proves SF will hand over partial damage when only a
  small area changes.
- Layer display frames are in the same **1440x720** space as the vendor's
  `update_area`, so no coordinate transform is needed (`xform=0`).
- The composer commits continuously even when nothing changed, so a "skip
  redundant commit" policy is available independently of rect support.

`vendor.debug.hwc.shim.test_w` / `test_h` force a fixed top-left rect and
`ioctl=` in the commit line times the kernel call itself, which separates panel
time from present cadence.


## Test procedure

### 0. Prereqs

TD A16 booted, `adb root` works (userdebug), and the vendor library staged:

```powershell
adb pull /vendor/lib64/hw/hwcomposer.mt6765.so build/hwc-shim-mt6765/real/
```

`deploy.ps1` compares that file's sha256 with the device's before it mounts
anything, so a mismatch is reported rather than silently swapped.

### Staging mode (verified on device, 2026-11-20)

Android's linker namespaces decide where the shim may `dlopen` the real library
from. On this phone the composer runs as `hal_graphics_composer_default` and
`/linkerconfig/ld.config.txt` `[vendor]` section permits **only** `/odm`,
`/vendor`, `/system/vendor` — so `/system/xbin` and `/data` cannot be dlopened
from, no matter what the SELinux label is. `/vendor` is a read-only ext4 mount,
so the module directory is overlaid instead:

1. copy `/vendor/lib64/hw` (7.5 MB, 46 files) to `/system/xbin/hwmirror` (tmpfs),
2. add the shim as `hwcomposer.mtk_common.so` (`variant` layout — the loader tries
   `ro.hardware.hwcomposer` = `mtk_common` and falls through to `mt6765` if the
   shim does not load, so a broken shim degrades to the vendor composer),
3. `mount --bind /system/xbin/hwmirror /vendor/lib64/hw`,
4. restart the composer.

Two things learned the hard way:

- **Per-file SELinux labels must be preserved.** Four modules
  (`gralloc.rogue.so`, `gralloc.default.so`,
  `android.hardware.graphics.mapper@4.0-impl-mediatek.so`, `vulkan.mt6765.so`)
  are `same_process_hal_file`, not `vendor_file`. A blanket `chcon -R vendor_file`
  makes SurfaceFlinger abort at startup with `no suitable EGLConfig found` in
  `SkiaGLRenderEngine::chooseEglConfig`, after which SF crash-loops and the
  framework never comes back. `device/apply.sh` copies each file's own context.
- **Never `rm -rf` the mirror while it is mounted.** A plain `umount` can fail
  with `EBUSY`; deleting through the live mount empties `/vendor/lib64/hw`.
  `apply.sh` detaches with `umount -l` and verifies before touching anything.

### Restart recipe (verified)

Restarting the composer kills SurfaceFlinger, and the Java side cannot survive a
stale `SurfaceControl` state: `system_server` dies ~90 s later with cascading
`InputChannel is not initialized` / `mNativeObject of Surface ... is null`
crashes, and wireless adb disappears with it (the framework serves it).

Restarting zygote deliberately right after the composer keeps it clean and does
not touch adbd:

```
setprop ctl.restart vendor.hwcomposer-2-1
sleep 3
setprop ctl.restart zygote
```

`device/watchdog.sh` reboots the phone if the framework is not stable 10 s later,
so a bad attempt cannot strand it. `adb` itself is made reboot-proof with
`setprop persist.adb.tcp.port 5555`.

### 1. Passthrough smoke test (no rewrite)

```powershell
pwsh -File deploy.ps1 -Disable
adb shell 'logcat -d -s hwcshim'
```

Expect: `disabled: pure passthrough`, display unchanged, `dumpsys SurfaceFlinger`
normal. If this fails, stop — labels or the real-library path are wrong (look
for `avc: denied` in `logcat -b all`).

### 2. Instrumented run — the run that decides everything

```powershell
pwsh -File deploy.ps1 -Rect 0 -Log 1 -LogEvery 30
adb shell 'logcat -d -s hwcshim' > hwcshim-observe.log
```

Read the log for:

- `commit #N dt=...us mode=0x.. area=(0,0,1439,719) ct=(...) layer=(...)` —
  `dt` is the real per-commit interval (panel scan); `ct`/`layer` are the damage
  rectangles SurfaceFlinger supplied, **in display space**.
- `setClientTarget ... damage=N` near the top — how many damage rects SF sends
  and whether it ever sends a partial one.
- `present #N ... client=/device=` — whether SF is really forcing client
  composition.

Decision:

- If `ct` is a proper partial rectangle → set `rect=1` and the right `xform`.
  Compare `ct` extents against the 1440x720 controller space: if `ct` never has
  `x > 719` while the vendor rect does, a 90° transpose is involved (`xform=1`);
  if the band lands on the wrong edge, try `2`/`3`.
- If `ct` is always the full panel → SF is not giving useful damage. Use `src=2`
  (layer damage, usually the full layer while an app animates) and, in fast mode,
  `fast_diff=1` for a true pixel diff of the client target.

### 3. Partial-rect rewrite

```powershell
pwsh -File deploy.ps1 -Rect 1 -Xform 1 -Log 1 -LogEvery 30
```

Watch for: commits with a small `area=`, `dt` dropping well below 40 ms, and
correct (ghost-free) content. A rectangle in the wrong space shows up instantly
as stale bands.

### 4. Throughput

- `BenchmarkActivity` (E-ink Center) for 10 s, count `[normal commit]` lines in
  `logcat -s hwcomposer`, compare with the 22–24/s baseline.
- Settings scrolling and a KOReader page turn for realistic damage.

Success criterion: commits/s above ~40 on partial content with no visible
artifacts; full-screen animation may stay limited by the panel.

### 5. Fast path — the run that matters

Do step 2 first and read `fmt=0x.. stride=.. h=..` on a vendor commit: those
describe the buffer the vendor converts. Expect `fmt=0`. Then:

```powershell
# 5a. Correct mode: our converter only (~28-30 fps, correct image)
pwsh -File deploy.ps1 -Fastconv 1 -Log 1 -LogEvery 40
#     skipcopy commits black frames on this GSI -- never use it as a speed mode
#     (deploy also sets the stock vendor.xrz.* props; apply.sh writes
#      eink_debug/clean_a2 + anti_flicker, both 0 on the GSI)

# 5b. static-screen check: should be ~0 commits/s
#     adb shell 'am start -a android.intent.action.MAIN -c android.intent.category.HOME'

# 5b. own the commit: vendor worker throttled to 2 Hz, shim commits
pwsh -File deploy.ps1 -Fast 1 -VendHz 2 -Log 1 -LogEvery 30

# 5c. static screen: no updates at all
pwsh -File deploy.ps1 -Fast 1 -FastDiff 1 -VendHz 2 -Log 1

# 5d. DO NOT USE as a performance mode: RGBA to the kernel is 13-258 ms/commit.
#     Kept only to re-test a future kernel.
pwsh -File deploy.ps1 -Skipconv 1 -Log 1
pwsh -File deploy.ps1 -Fastgray 1 -Log 1
```

Read:

- `fast #N rgba [l,t,r,b] mode=0x.. fmt=.. stride=.. ioctl=..us ret=0` — one line
  per `log_every` commits. `ioctl` is the whole cost now; `ret=0` is required.
- `skips=` in `dumpsys SurfaceFlinger` — frames skipped because nothing changed.
- `fails=` — fence timeouts and ioctl errors. A rising count means the fast path
  keeps falling back; the line above it says why.

Expected: ≥40 `fast #` commits/s on scrolling content and ~0 commits when the
screen is static, with correct text and UI. Stop and revert if the content is
wrong with both `-Fast 1` and `-Fast 1 -Inplace 1`; that means this kernel does
not take the client target and the vendor path is the only one.

---

## Risks

- A broken shim can blank the display or crash the composer. `adb reboot`
  restores everything: nothing is written to a persistent partition, the swap is
  a bind mount and `/system/xbin` is tmpfs.
- The vendor may ignore a shortened rectangle for some modes (GC16 / clean), in
  which case only the fast modes gain. That is a `mode` knob in this shim, so it
  is measurable.
- Locked bootloader: keep to the system/vendor userspace. No kernel, no boot.
