# Status — 2026-11-24

## Measured on the device (TD A16 GSI, full panel, `format=0`, `ret=0`)

| configuration | processImage | commit `dt` | commits/s | note |
|---|---|---|---|---|
| vendor, untouched | 37 ms | 34 ms | ~27 | RGBA→grey conversion + 4 MB copy |
| `-Fastconv 1` (our converter) | 35 ms | 34 ms | ~28 | correct, safe; converter ~2.3 ms, copy dominates |
| `-Skipcopy 1` | 3–4 ms | 16 ms | 63 | **black frames** — removes the converter's input |
| `-Skipcopy 1 -CtSrc 1` | 3–4 ms | — | — | crashed the composer (mapper lock in gralloc) |
| `-Skipconv 1` (RGBA to kernel) | 0.04 ms | 235 ms | ~4 | kernel RGB→grey is slower |
| `-Fast 1 -Ownbuf 1` (own ION buffer) | 3–4 ms | 85 ms | ~12 | ION writes ~80 ms (uncached) |

`fastconv` alone is the only configuration that is both faster than the vendor on CPU
and visually correct. The interval stays ~34 ms because of the 4 MB staging copy.

## Proven

- The vendor composer spends 31–80 ms (avg 33.5 ms) in a `memcpy` of the client target
  into a staging buffer; `GraphicBuffer::lock` (50–115 µs), `sync_wait` (~30 µs) and
  the 4.1 MB `memcmp` (24–40 µs) are not the cost.
- Our NEON converter is correct: 0.35/255 mean difference vs the vendor's, max 29,
  no pixel off by >32.
- The driver accepts commits every 12.5–17.4 ms (`ret=0`) once the copy is out of the
  way, so ~60 Hz per commit is possible.
- `clean_a2=1` + `anti_flicker=1` + the stock `vendor.xrz.*` properties fix the
  ghosting/stutter that made fast modes unusable; E-ink Center applies them at boot.

## Rejected (measured, do not retry)

- `skipcopy` as a speed mode: black frames.
- RGBA to the kernel (`skipconv`): 13–235 ms per commit.
- In-place grey on the client target: 90 ms of uncached writes.
- Private ION destination: 80 ms per 1 MB write, even from `ion_system_heap` with
  `ION_FLAG_CACHED|CACHED_NEEDS_SYNC`.
- `GraphicBufferMapper::importBuffer`+`lock` of the client target inside the composer:
  SIGSEGV in `gralloc.rogue.so` (fault at 0x8) on the e-ink worker.

## Known caveats

- `deploy.ps1` sets knobs with `adb shell setprop` and ignores failures; on a flaky
  wireless link the old values can persist and the next start runs a stale config.
  Read them back after deploy.
- The e-ink debugfs nodes are root-writable only; the app writes them at boot.
- Vendor binaries are not redistributed here.
