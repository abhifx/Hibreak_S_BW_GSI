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
| `skipcopy` | 0 | 1 = drop the vendor's 4 MB staging copy — **unsafe: commits black frames; do not use as a speed mode** |
| `xproc` | 0 | 1 = use the out-of-process gray helper |
| `xproc_vendor` | 0 | 1 = helper feeds vendor gray buffer; vendor performs physical commit (visible 60 fps) |
| `xproc_damage` | 0 | 1 = opt-in partial GPU conversion when valid CT damage is available; current GSI exposes none, so keep 0 |
| `helper_onebit` | 0 | 1 = force B/W helper output; 0 = 16-level gray (default, preserves Hyper gradients) |
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

## 2026-11-26 — exact vendor destination + empty-cgroup reclamation

- `hook_mtk_commit` no longer rewrites `cfg->fd`.
- `hook_gb_lock` matches the exact vendor output `GraphicBuffer` when
  `GraphicBuffer::lock(usage=0x30)` runs inside the active `processImage` job, then
  resolves that object's `CommitBuffer+0x18` fd under `OverlayEngine+0x428`.
  No round-robin destination guess and no `cfg->fd` rewrite.
- A timed-out helper job quarantines that dma-buf identity for 8 seconds; the
  vendor converter remains in charge for the quarantined destination.
- The helper uses a persistent abstract-socket connection and cached output
  EGLImages/textures/FBOs. Input/output GL resources are released on failure.
- A real `ct_map` fallback leak was fixed: retained ION imports and their map fds
  are now freed on cache eviction/error.
- Empty per-PID cgroup2 directories are reaped by `helper-watchdog.sh`, E-ink
  Center (boot/input/screen transitions), and provisioning. Removing 1,011 stale
  cgroups dropped `SUnreclaim` by ~976 MB in the same boot; full cleanup later
  reached ~280 MB with ~2.62 GB `MemAvailable`.
- E-ink Center's `Root.run`, frontlight writes, and touch-node lookup now share
  the persistent root shell rather than spawning new root processes.
- Boot/provisioning reclaim empty cgroups and retain the current Home app. They
  no longer automatically disable GMS/GSF/Vending or force-stop TrebleApp/Launcher3.
- The exact vendor destination is resolved from the output `GraphicBuffer` during
  `lock(usage=0x30)` for the active job, not guessed from rotating pool pointers.
- `CtMap` cache slots are initialized with `ion_handle=-1`/`map_fd=-1` so an
  unused slot cannot accidentally free handle/fd 0.
- E-ink Center default navigation is one transparent 28 dp (48 px) non-touchable
  gesture strip with an 8 dp handle. Static overlay ownership prevents duplicates
  across accessibility reconnects; three reconnect cycles retained one window.
  The legacy 64 dp three-button bar remains opt-in.
- Full refresh restores the prior refresh mode after its one-shot GC16 request.
- Final clean results: Fast 10 s **581 commits (58.1/s)**; Hyper 60 s **3523
  commits (58.72/s)**; post-recovery Hyper 10 s **588 (58.8/s)**; post-wake Hyper
  10 s **595 (59.5/s)**; final post-E-ink-Center Hyper 10 s **587 (58.7/s)**. All
  had zero fallbacks, quarantines, and crashes. The 60 s soak kept `SUnreclaim`
  ~343 MB, helper RSS 56,496 kB, and fd/dma-buf counts bounded. Prior
  overlapping-run counts are discarded.

## Fast path (2026-11-25) — visible 60 fps via vendor-pipeline helper

**Visible 60 fps is achieved in both Fast and Hyper modes with `xproc_vendor=1`.**
The latest candidate binds a source snapshot at worker-job start, waits its fence,
matches the vendor's exact output `GraphicBuffer` during
`GraphicBuffer::lock(usage=0x30)`, and sends that object's `CommitBuffer+0x18` fd to
the helper. `hook_mtk_commit` keeps the vendor's original fd unchanged. A helper
failure uses a same-frame snapshot fallback into that vendor buffer; timed-out
destinations are quarantined, and sustained stalls trigger
`helper-watchdog.sh` recovery. Final clean soak results are recorded above;
older fd-redirection measurements are obsolete.

The GPU helper writes packed gray directly into the vendor's own gray commit fd(s);
the vendor worker keeps its normal `processImage`/`triggerEinkCommit`/`mtk_commit`
path, so the physical panel, touch input, grayscale, and ghost cleaning continue to
work.

Measured Fast: 1886 physical vendor commits/30 s (62.9/s), 639/10 s (63.9/s).
Measured Hyper with 16-level gray: 633/10 s (63.3/s). Choreographer 120–124/s,
zero fallbacks. SELinux remains enforcing.

The follow-up build clones the `GB01` native handle under the snapshot lock for
each vendor conversion, fixing a use-after-free when the next client target
arrived. It measured 610/10 s (61/s) and 1851/30 s (61.7/s) in Hyper with zero
fallbacks and no composer restart. The profiling-only `memcmp` hook was later
removed after a stress crash; the current build measured 602/10 s and 1794/30 s
(~59.8/s) with no crash and a stable composer PID.

The helper is now restricted to Fast/Extreme/Hyper/A2. HD 256 and Regal use the
full vendor conversion/copy path because skipping the staging copy can crash
the vendor in those quality modes. HD 256 passed 20 repeated shade cycles;
Hyper measured 602/10 s with zero fallbacks.

Helper failures now clear the ready flag immediately, so the vendor staging copy
resumes instead of feeding stale data to the fallback converter. A five-second
watchdog restarts a dead helper. A controlled outage test kept the composer alive
and restored the helper automatically; Hyper then measured 593/10 s with one
safe fallback.

The vendor can also call a 4 MB `memcmp` with the sentinel pointer `-1`. A guarded
hook rejects only clearly invalid large-buffer pointers, preventing the
`processImage` SIGSEGV. A 60 s Hyper soak measured 3420 commits (57/s), zero
fallbacks, unchanged composer PID, and no crash; HD 256 passed 20 shade cycles.

### Working deployment

`deploy.ps1` now defaults to this known-good profile. Pass
`-Xproc 0 -XprocVendor 0` explicitly for a vendor-only deployment.

```powershell
pwsh -File build/hwc-shim-mt6765/build-all.ps1
pwsh -File build/hwc-shim-mt6765/deploy.ps1 `
  -Fast 0 -Xproc 1 -XprocVendor 1 -HelperOnebit 0 -Mode -1 `
  -Nofence 0 -EinkVsync 1 -Log 1 -LogEvery 60 -Watchdog 90
adb push tools/measure-xproc-vendor.sh /data/local/tmp/measure-xproc-vendor.sh
adb shell chmod 755 /data/local/tmp/measure-xproc-vendor.sh
adb shell sh /data/local/tmp/measure-xproc-vendor.sh 10 178 1

# Hyper with 16-level gray (third argument forces the mode):
adb shell sh /data/local/tmp/measure-xproc-vendor.sh 60 183 1
```

The measurement script serializes runs and temporarily removes only E-ink
Center's accessibility component so its per-app mode controller cannot override
the requested mode. It restores the component and all properties/settings on
exit, and fails if benchmark frame logs or physical commit logs are missing.

### Damage-aware helper status

Protocol v2 can carry a job-bound damage rectangle. The helper keeps a packed-gray
accumulator, updates up to four scissored GPU bands, and always copies the complete
accumulator into the exact rotating vendor buffer. Standalone tests are byte-exact
against a full-frame reference for both 16-level and 1-bit output.

The current GSI passes zero client-target damage rects and only full-screen layer
damage, so there is no safe partial rectangle to use. `xproc_damage` therefore
defaults to 0 and the production path remains full-frame. The opt-in code is kept
for a future damage-capable SurfaceFlinger/HWC path.

Perfetto tracing then identified two safe full-frame optimizations:
`debug.sf.luma_sampling=0` removes unnecessary RegionSampling compositions, and a
four-entry input cache eliminates repeated gralloc imports. The final 60 s Hyper
soak reached 3593 commits (59.88/s) with zero fallbacks and flat memory/fd/RSS;
post-reboot Hyper measured 598/10 s (59.8/s).

The helper uses an abstract socket, raw `GB01` input transfer, GLES2 packed-gray
conversion, and a raw vendor-output fd (`HWO1` + `SCM_RIGHTS`). Four imported
client targets are cached by dma-buf identity, eliminating repeated gralloc
import/free work. The persisted profile also sets `debug.sf.luma_sampling=0`
because brightness is manual; this removes SurfaceFlinger's extra RegionSampling
compositions and reduces buffer pressure. Together these changes reduced helper
conversion to ~11.1 ms and warm Settings 90th-percentile frames to ~30 ms.

### Boot persistence

`deploy.ps1` writes `persist.env`, `persist.layout`, and `persist.watchdog`, then
installs `device/persist.sh`. E-ink Center's existing boot receiver launches the
script on its retry ticks. The script is idempotent per boot, verifies SELinux
and the approved vendor ABI, restores the saved properties, starts the helper and
watchdog, reapplies the bind mount, and verifies the display stack before writing
`persist.boot`.

A normal redeploy re-enables persistence. `deploy.ps1 -Revert` creates
`persist.disabled`, detaches the overlay, and leaves it disabled across reboots.
To disable manually without detaching immediately:

```sh
adb shell touch /data/local/tmp/hwc-shim-mt6765/persist.disabled
```

The next reboot then stays on the vendor composer. A normal deploy removes the
marker and re-enables boot re-application.

**Do not use the older direct mode** `-Fast 1 -Xproc 1`: it commits the helper's
separately allocated buffer and produces fast software calls, but the physical
panel stays frozen. Also do not use `skipcopy` as a speed mode; by itself it
commits black frames. The vendor-only `fastconv=1` mode remains the safe
~28–30 fps fallback.


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

- A broken shim can blank the display or crash the composer. With persistence
  enabled, reboot reapplies the same verified bind mount. To return to the vendor
  composer across reboots, run `deploy.ps1 -Revert` (it writes `persist.disabled`)
  or create that marker manually. No boot/kernel/vendor partition is modified.
- The vendor may ignore a shortened rectangle for some modes (GC16 / clean), in
  which case only the fast modes gain. That is a `mode` knob in this shim, so it
  is measurable.
- Locked bootloader: keep to the system/vendor userspace. No kernel, no boot.
