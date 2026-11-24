# Fast refresh on the GSI: own the commit, don't optimise the vendor

**2026-11-23. Status: implemented in `hwcshim_mt6765.cpp` (`fast=1`), built, not
yet run on the HiBreak.** This supersedes the "partial rectangle" plan as the
primary route: the rectangle was never the bottleneck.

> **2026-11-24 — CORRECTED: ~28-30 fps is the ceiling here; the earlier 63/s was black.**
>
> The 63-64 commits/s measurement was real, but the panel was **black**:
> `skipcopy` removes the input to the vendor's converter (its staging buffer),
> so the converter produced empty frames at 63/s. Raw `mmap` of the client target
> is uncached (80 ms per 4 MB) and `ion_import` is rejected, so the client target
> cannot be read at speed; the vendor's 4 MB staging copy (31-80 ms, avg 33.5 ms)
> is mandatory.
>
> Correct mode: `fastconv=1` alone (our converter 2.3 ms, verified 0.35/255) plus
> the stock `vendor.xrz.*` properties and `eink_debug/clean_a2` — **~28-30 fps,
> correct image**. Stock/Pro reach 60 only because their SF pre-converts to
> 1-bit grey (their patch 0013), so the composer copies 1 MB.
>
> The paragraphs below are the earlier analysis; the mechanisms are real, but
> no userspace HWC trick removes the 4 MB copy. 60 fps needs the SF-side render
> pass (system image build).
>
> | configuration | processImage | commit interval | verdict |
> |---|---|---|---|
> | vendor, untouched | 37 ms | 34 ms | baseline |
> | `fastconv=1` (our NEON converter) | 35 ms | 34 ms | safe, small CPU win |
> | `fastconv=1 skipcopy=1` | **3–4 ms** | 16 ms per commit | **unsafe** |
> | `fastconv=1 novsync=1` | 35 ms | 34 ms | no gain while copy-bound |
> | `skipconv=1` (RGBA to the kernel) | 0.04 ms | 235 ms | rejected |
>
> The 32 ms is a **4 MB `memcpy`** (31–80 ms) of the client target into a staging
> buffer, which the vendor's frame-difference bookkeeping needs. The converter is
> *not* the main cost: the vendor's own is comparable, and ours (2.3–2.6 ms,
> verified to 0.35/255 mean difference) only saves a few ms.
>
> `skipcopy=1` removes the copy and drops `processImage` to 3–4 ms, but the app
> then renders at 62 fps while the composer commits ~3/s — the vendor's change
> detection is defeated and the panel goes stale. **Do not use it.**
> `novsync=1` only helps once the composer is fast (16 ms per commit, measured
> with an 80-row rect); with the copy in place the interval stays ~35 ms.
>
> Real remaining fix: own the commit (the `fast=1` path) with a **private gray
> buffer** (ION/gralloc allocation), which removes the copy and the vendor
> bookkeeping together. The `inplace` fallback is not usable: writing into the
> client target's uncached dma-buf costs 90 ms.

>
> So the 32 ms was **not** the conversion: it was a redundant 4 MB `memcpy`
> (`31–80 ms`, avg 33.5 ms) that copies the client target into the commit buffer
> and is then overwritten by the converter. The converter itself was also slow
> (37 ms); ours is **2.3–2.6 ms** and matches the vendor's output to a mean of
> **0.35/255**.
>
> Winning mode: `fastconv=1 skipcopy=1` — vendor pipeline, mode, rectangle and
> format untouched, only our converter and no redundant copy. The remaining ~30 ms
> interval is *pacing*, not CPU (an 80-row rectangle costs the same ~30 ms as a
> full one), and `novsync` is being tested. Do not use `skipconv`/`fastgray`:
> handing RGBA to this kernel costs 13–258 ms per commit.


---

## 1. What the measurements actually say

From `FINDING-cpu-bound.md` (2026-11-20, instrumented on the TD A16 GSI):

```
commits/10 s = 267                 26.7 commits/s
pre (present -> mtk_commit)        median 32.6 ms   <- inside the composer
dt  (commit -> commit)             median 35.6 ms
ioctl (mtk_commit -> return)       median  2.7 ms
composer CPU                       96.8 %, Dispatcher_0 alone 103 %
app render rate                    54 fps (with the fence removed)
```

Three conclusions follow, and they decide the design:

1. **The panel is not the limit.** A 35.6 ms interval made of 32.6 ms of user
   CPU and 2.7 ms of ioctl leaves ~0.3 ms for the panel. If the scan-out were
   the floor, `rect`/`height`/`mode` experiments would have changed it; they did
   not (same finding).
2. **SurfaceFlinger is not the limit.** It presents at ~54 fps while the
   composer commits at 26.7/s, so SF is already able to feed us twice as fast.
3. **The vendor's worker is the limit, and it is asynchronous.** `presentDisplay`
   returns first; the 32.6 ms is burnt afterwards on another thread
   (`OverlayEngine::processImage` / `convertLayerToGray`) re-converting the
   already-composited client target from RGBA to 16-level gray.

So the rectangle rewrite can only ever help *after* the 32 ms is gone. The 32 ms
is not partially attributable to the panel: it is one core doing redundant work,
and that same core is needed by SF/RenderEngine for the next frame.

## 2. The idea

Do not make the vendor's worker faster. **Stop using it.**

On this GSI SurfaceFlinger is forced into client composition (the
`SurfaceFlinger 1008` setting), so the client target *is* the final frame. It
arrives at `setClientTarget(display, target, acquireFence, dataspace, damage)`
together with everything needed to display it:

| needed | where it comes from |
|---|---|
| dma-buf fd | `gralloc_extra_query(handle, 1)` (fallback `handle->data[0]`) |
| width / height / stride / format | `gralloc_extra_query(handle, 10/11/12/15)` |
| "the GPU has finished writing it" | the acquire fence argument |
| what changed | `setLayerSurfaceDamage` (this HWC 2.1 build never fills CT damage) |

The shim therefore builds `eink_update_config` itself **in `presentDisplay`** and
calls `mtk_commit` directly. That is the whole trick:

* no 32 ms conversion (the kernel converts RGBA->gray, as it does on B1051C with
  `commit_use_ct`; `inplace` is the fallback if this kernel does not),
* no 35 ms worker cadence (we commit in the present call itself),
* no full-panel rectangle (we commit the damage/diff rectangle),
* and the vendor's worker is left to do nothing: its `mtk_commit` calls are
  dropped, and `vend_hz` stretches its vsync wait so it stops pinning a core.

Expected result, if the kernel accepts the RGBA client target: per frame =
fence wait (usually already signalled) + ioctl (2.7 ms, a little more for 4x the
bytes read) + panel scan-out in parallel. That is SF's own cadence, i.e. 50-60
updates/s for partial content, instead of 26.7.

## 2b. The vendor library already contains the fast path (found in the disassembly)

This is the cleaner half of the fix, and it is not a guess. In `hwcomposer.mt6765.so`:

```
OverlayEngine::convertLayerToGray(param, ci)              0xa1260
  w27 = *(u32*)(ci + 0x54)                               0xa12c4   <- format
  ...
  cbnz w27, 0xa14c4                                      0xa1488   <- format != 0: SKIP
  bl   xrz::EffectHandler::convertToGray(dst, dst, ...)   0xa14c0   <- in place, on the CT

ComposerHandler::process(job)                             0xae668
  *(u32*)(ci + 0x54) = *(u32*)(job + 0x8)                 0xae7f8   <- format comes from the job

DispDevice::triggerEinkCommit(ci)                         0xf52a0
  cfg.format = *(u32*)(ci + 0x54)                         0xf58f8/0xf5970
  cfg.fd     = graphicbuffer->fd
```

So the vendor's own code says: **`format != 0` means "the buffer is RGBA, the
kernel converts it, do not convert here"**, and `format == 0` means "gray8,
convert in place". On this GSI the job field is 0, so the 1440x720 client target
is converted in place on one core, 27 times a second: the 32 ms.

`processImage` is called through the PLT (`bl 0x119770`, slot at 0x1233f8), so
the existing GOT hook can write that one field before the conversion runs:

```
vendor.debug.hwc.shim.skipconv=1   ->  *(u32*)(ci+0x54) = 1   in hook_processImage
```

That removes the 32 ms **inside the vendor's pipeline**, keeping its commit
thread, present fences, mode selection, power handling and GC16 logic. Nothing
else is bypassed, and the only unknown left is again whether the kernel does the
RGBA->gray conversion (see 4.1). If it does, this alone should take the composer
from 26.7 to its present cadence.

`fast=1` and `skipconv=1` are independent:

| | `skipconv=1` | `fast=1` |
|---|---|---|
| who commits | vendor worker | shim, in `presentDisplay` |
| conversion | skipped in the vendor | skipped entirely (worker is idle) |
| rectangles | vendor policy, `rect`/`fast_diff` optional | shim damage + pixel diff |
| fences/power/GC16 | vendor, untouched | shim (present fence dropped) |
| risk | one struct field | whole commit path |

Try `skipconv=1` first: it is the smaller change and the more likely daily
driver. Use `fast=1` if the vendor's remaining per-frame work is still too slow,
or to get the pixel-diff idle behaviour.

One extra lever found in the same function: `processImage` normally allocates a
`CommitBuffer` and copies the client target into it before converting. If
`vendor.xrz.disable_image_copy` is non-zero (or the mode is `0xb5`), it skips
that copy and wraps the client target's own `GraphicBuffer` as the commit buffer
(`0x9fd94`). Combined with `skipconv=1` that removes the copy as well:

```powershell
adb shell setprop vendor.xrz.disable_image_copy 1
```

It was inert in the 2026-11-20 matrix because the conversion still dominated.
Use it only after `skipconv` is known good: committing the client target directly
means SF can reuse it while the panel scans, so it trades a little tearing risk
for the copy time.



## 3. Why this is safe to try

* Nothing is written to a persistent partition. The shim is a bind mount; one
  `adb reboot` restores the vendor composer.
* Every uncertainty has a knob, and each one degrades to the vendor path on
  failure: an unusable fd/format, an unsignalled fence, an ioctl error, or a
  panel-off state all clear `fast_active`, and the vendor's worker (whose calls
  are then no longer dropped) commits again.
* The pixel diff (`fast_diff=1`) is computed against a shadow copy, so a static
  screen produces **no** panel updates at all, not 26/s. That is a large win on
  its own even if the throughput gain disappoints.

## 4. The two open questions, and what answers them

### 4.1 Does this kernel convert RGBA, or does it want 8 bpp?

The vendor's own code answers the *meaning* of the field: `format == 0` means
gray8/convert-on-CPU, `format != 0` means RGBA/let-the-kernel-convert (0xa1488).
What is still unverified is whether this kernel implements that RGBA path at
all — the ioctl here (`0x40444FE4`) is not B1051C's `0xC04464CF`, even though the
struct is the same shape.

**Read the observe log first.** One vendor commit line prints the vendor's own
answer:

```
commit #N ... fd=<fd> fmt=0x<F> stride=<S> h=<H> dither=0x<D>
```

With the GSI's default path `F` should be `0` and `S == 1440` (gray8, 1 B/px).
Any non-zero value is the "RGBA" value the kernel expects; start with `1`
(HAL_RGBA_8888) via `skipconv_fmt` / `ct_format`, and try 2/4 if the panel is
garbled.

Failure mode if the kernel does not convert: the RGBA buffer is read as 8 bpp,
so the frame is squeezed into the first quarter of the buffer — the classic 4x
tiling B1051C saw. Immediately obvious. Use `inplace=1` (fast path) in that case.


### 4.2 Does the kernel read the client target buffer where we think?

It is the same buffer the vendor's CPU conversion reads (proof: the vendor
converts it on the CPU), so it is linear and CPU-reachable. Two residual risks:

* **AFBC/tiling** would break every CPU read; because the vendor converts it,
  it cannot be tiled. Cheap sanity check: `fast_diff=1` logs a plausible bbox
  for a small on-screen change instead of "whole panel".
* **buffer rotation / reuse while scanning.** SF rotates client-target buffers
  (each `setClientTarget` shows a different fd); three slots give the driver
  ~50 ms at 60 fps, longer than a full-panel scan. If tearing appears anyway,
  cap with `fast_hz=30` or use `inplace` (smaller scan, faster release).

## 5. What is deliberately given up in fast mode

* **App-requested full refresh / GC16** (E-ink Center) goes through the vendor's
  worker, and its commits are dropped while fast is active. Set `fast=0` for a
  clean full refresh, or extend `hook_mtk_commit` to let non-Hyper/A2 modes
  through (not done: the mode numbers for GC16 on this library are unknown).
* **The vendor's dithering / image smoothing.** The kernel path dithers if the
  driver does; `inplace` does a 4x4 ordered dither to 16 levels. HD/GC16
  photographs will look worse than stock; text and UI are unaffected.
* **Automatic mode selection.** The fast path mirrors the vendor's last mode
  (`0x11` Hyper before the first vendor commit), or `mode=` forces one.

## 6. Test sequence (when the HiBreak is connected)

```powershell
# 0. baseline: observe only, read fmt/stride and where the damage is
pwsh -File deploy.ps1 -Rect 0 -Log 1 -LogEvery 30
adb shell 'logcat -d -s hwcshim' > observe.log     # look for "fmt=0x.. stride=1440"

# 1a. smallest change: the vendor skips its own conversion, kernel converts
pwsh -File deploy.ps1 -Skipconv 1 -Log 1 -LogEvery 30
#     expect "skipconv: CommitInfo format 0 -> 0x1" and dt collapsing

# 1b. if the panel is garbled with 1a, the kernel wants 8 bpp: try the values
pwsh -File deploy.ps1 -Skipconv 1 -SkipconvFmt 2   # then 4; or revert to fast+inplace

# 2a. fast, kernel converts RGBA, vendor worker throttled to 2 Hz
pwsh -File deploy.ps1 -Fast 1 -VendHz 2 -Log 1 -LogEvery 30 -Nofence 1

# 2b. if the panel is garbled: same thing, convert in place to gray8
pwsh -File deploy.ps1 -Fast 1 -Inplace 1 -VendHz 2 -Log 1

# 3. static screen: no updates at all (fast path only)
pwsh -File deploy.ps1 -Fast 1 -FastDiff 1 -VendHz 2 -Log 1

# 4. revert
pwsh -File deploy.ps1 -Revert      # or: adb reboot
```

Read `dt=` on the vendor commit lines: with `skipconv=1` it should fall from
~35 ms to a few ms even before any rectangle work. Then add `-Rect 1` to shorten
the scan.

Success: `fast #N ... ioctl=...us` (or vendor `dt=`) at ≥40/s with correct
content, and `skips` rising on a static screen. Stop rule: if the panel is not
correct with `skipconv` (all `skipconv_fmt` values) and `fast`, and not with
`fast+inplace`, the kernel does not accept the client target and the vendor gray
path stays as the only correct one.

## 7. If this works, what is left

* A proper `sw_sync` retire fence tied to panel completion, so SF never rewrites
  a buffer still being scanned (removes the tearing risk entirely).
* Mode policy: Hyper while scrolling, HD/GC16 when the screen settles, one
  settle refresh after motion stops (ghost cleanup).
* Let GC16/full-refresh requests through the suppression.
* Only then revisit per-layer damage precision (`source crop`, transforms) —
  with a 1 ms commit path, rectangle precision is a polish item, not the
  bottleneck.
