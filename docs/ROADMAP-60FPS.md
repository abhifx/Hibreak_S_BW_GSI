# Roadmap to 60 fps

The composer can already accept a full frame every 12.5–17.4 ms (`ret=0`). What it
cannot do is *receive* the frame cheaply: the GSI's SurfaceFlinger hands it 4 MB of
RGBA, and the vendor pipeline moves those 4 MB through a staging buffer (~33.5 ms).
Everything below attacks that, in order of expected payoff.

## Task 1 — SurfaceFlinger e-ink render pass (system image)

**Goal:** the composer receives a frame that is already what the panel shows, and
only the damaged region is copied.

Take the published patches from the HiBreak Pro LineageOS project and rebase them onto
the GSI's Android 16 branch:

- `frameworks/native/0013-sf-e-ink-render-pass...` — the render pass itself
  (`DisplaySettings`, `SkiaRenderEngine`, `Output.cpp`, blue-noise map).
- `frameworks/native/0004` — client-target damage (`debug.sf.hwc_client_target_damage`).
- `frameworks/native/0002` — explicit full damage instead of zero rects
  (`debug.sf.hwc_explicit_full_damage`); the vendor composer treats zero rects as
  "no change" and skips the commit.
- `frameworks/native/0006` — A2 while the screen moves (`debug.sf.eink_motion`).

Build a system image (see `BUILD-GSI.md`) and install as a DSU.

**Acceptance:** `dumpsys SurfaceFlinger` shows the e-ink render pass active; the
composer's `processImage` drops below ~10 ms with client-target damage non-empty;
scrolling text commits at ≥50/s.

**Open questions:** client-target pixel format the vendor composer accepts without a
4 MB copy; whether the composer copies only the damaged region when damage is present;
adapting the pass to 720x1440 and the mono panel.

## Task 2 — validate the client-target mapping, safely

The mapper read is the fastest path measured (2.2 ms / 4 MB through
`GraphicBufferMapper::importBuffer` + `lock`, measured out-of-process), but doing it
inside the composer crashes `gralloc.rogue.so`. Work out *why* and whether another
usage/format/import path is safe:

- reproduce in a small harness process (the Pro project's `gpu-gray --xproc` idea),
- compare usage flags (`SW_READ_RARELY` vs `SW_READ_OFTEN`, `HW_TEXTURE`),
- check whether the handle lifetime (buffer freed by SF) is the real cause, not the
  mapper itself,
- only then wire `-CtSrc 1` back in. Guard it so a crash cannot wedge the phone
  (watchdog + a knob that defaults off).

## Task 3 — partial updates (needs Task 1)

Once SF sends damage, the vendor composer should copy/commit only the band. Measure:

- scrolling Settings/KOReader: area, `dt`, commits/s;
- confirm the copy size follows the damage (`memcpy n=` in the shim log);
- tune the mode policy: Fast/Hyper while moving, 16-level when still.

## Task 4 — mode policy and ghosting

`clean_a2` fixed most of it. Remaining: pick the refresh mode per workload
(Fast for video, 16-level for text), and a settle refresh after motion stops.
The stock values live in the `vendor.xrz.*` properties; E-ink Center exposes them.

## Non-goals

- Unlocking the bootloader or flashing boot/vendor: not possible on this device.
- Custom waveforms: proprietary, and the panel is already driven by the vendor stack.
- Replacing the vendor composer wholesale: it owns the panel session; the shim only
  wraps it.

## What would falsify the plan

If a system image with the SF patches still copies 4 MB per frame and stays at ~28 fps,
then the copy is inside the vendor composer regardless of input, and the only remaining
option is the vendor's own pre-converted path (which stock uses) — i.e. the stock
framework, not a GSI.
