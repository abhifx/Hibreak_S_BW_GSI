
## The real bottleneck: CPU conversion, not the panel (2026-11-20 22:45)

A clean load (a minimal always-animating APK, `build/eink-bench/`) finally made
the measurement unambiguous:

* the app renders at **54 fps** with the fence change, 48-50 without;
* the **panel is only committed 25-28 times/s** either way (median 35 ms);
* the composer process sits at **96.8% CPU, with `Dispatcher_0` alone at 103%**
  = one core pinned;
* `mtk_commit` itself returns in **2.6 ms**.

Everything else is inert: rect size, `height`, `flag`, mode (`0xb5` forced to
`0x11` Hyper), `fps_high/fps_low`, HWC composition on/off (`SurfaceFlinger 1008`),
`disable_image_copy`, `image_smoothing_suppoort`, `color_restoration_enabled`,
`driver_color_enhance`, `gray_256_dither` — all leave the 35 ms interval
unchanged.

Conclusion: the vendor composer burns ~35 ms of single-threaded CPU per frame
converting the **full 1440x720 RGBA client target to 16-level gray**. TD forces
client composition (the `1008` hack), so that conversion happens for the whole
screen every frame. That is the ~24-28 fps ceiling — not the panel, not the
waveform, not the rectangle.

Stock runs the same `/vendor` binary but reaches ~15 ms/commit, so its
SurfaceFlinger must hand the composer something far cheaper to convert. That
matches the long-standing finding that Bigme's / HBPC SurfaceFlinger carries the
e-ink render/dither/damage work — the part that needs a source build.

## Confirmed by instrumentation (2026-11-20 23:05)

With the shim timing both `presentDisplay -> mtk_commit` and the ioctl, on a clean
full-screen animation in Hyper (`0x11`):

```
commits/10s = 267        (26.7 per second)
pre   (present -> commit)  median 32.6 ms   avg 34.4 ms
dt    (commit -> commit)   median 35.6 ms   min 35.0 ms
ioctl                      median  2.7 ms
```

So **32.6 of the 35.6 ms elapse inside the composer's worker thread, between
present returning and the commit** — that is exactly where
`OverlayEngine::processImage` / `convertLayerToGray` run. The kernel call is 2.7 ms
and the panel is irrelevant to the interval.

## The better fix: bypass the conversion, do not parallelise it

The B1051C project had the same problem and solved it differently: it hands
`mtk_commit` the **RGBA client target directly** (`commit_use_ct`) because the
kernel's e-ink driver converts to gray itself; the vendor's userspace conversion
was redundant. Their `drm_eink_update.fd/format/stride` rewrite is that trick.

Our `eink_update_config` has the same fields (`+0x28 fd`, `+0x2c format`,
`+0x38 stride`, `+0x3c height`), so the same approach applies:

1. capture the client-target fd in `setClientTarget` (gralloc_extra_query, or
   `native_handle->data[0]` as fallback);
2. hook `processImage` to return immediately (skip the conversion);
3. set the commit's `fd`/`format`/`stride` to the RGBA client target.

That removes the 32 ms instead of dividing it, and it is a proven trick on the
sibling platform. Risk: if this kernel driver does not convert, the panel gets
garbage — reversible in one knob.


Partial rects cannot help (the conversion is whole-frame). The only non-compile
route left is to **replace the conversion**: the B1051C shim already hooks
`OverlayEngine::convertToGray` / `processImage` for its colour pipeline, and the
same GOT-hook mechanism applies here. Doing RGBA->gray with 8 threads (or
feeding the commit a pre-converted buffer with the right format/stride) would cut
the 35 ms to single digits and move the limit back to the panel — where it
belongs. Next step is instrumentation first: time `presentDisplay -> mtk_commit`,
which isolates the conversion, before rewriting it.
