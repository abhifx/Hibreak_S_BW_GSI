# Hardware and vendor stack notes

Everything here was recovered from this device (BuildID
`0bc3c35a2ffd85ff39c768deb4b0230b` for `hwcomposer.mt6765.so`).

## Panel

- 5.84", 720x1440 mono e-ink; the e-ink controller buffer is **1440x720** (landscape),
  which is why every commit rectangle is logged as `(0,0,1439,719)`.
- Kernel 4.19.191, MediaTek MT6765. Vendor composer `/vendor/lib64/hw/hwcomposer.mt6765.so`.
- The vendor chooses the waveform mode; this library's encoding differs from the
  HiBreak Pro (MT6877): `0xb1` HD, `0xb2` Fast, `0xb3` Extreme, `0x11` Hyper, `0x10` A2,
  `0x800000b1` HD256. Boot default is `0xb2` (`ro.vendor.xrz.default_refresh_mode=178`).

## Commit path

```
DispDevice::triggerEinkCommit(CommitInfo*)              0xf52a0
  -> mtk_commit(eink_update_config)  (PLT)               -> ioctl(fd, 0x40444FE4, &cfg)
```

`0x40444FE4` = `_IOW(0x4f, 0xe4, 68)`. `eink_update_config` (68 bytes):

| off | field |
|---|---|
| +0x00 | `u32 mode` |
| +0x04..+0x10 | `left, top, right, bottom` (inclusive, controller space) |
| +0x14 | `u32 dither` |
| +0x18 | `u32[4]` zero |
| +0x28 | `s32 fd` (source buffer) |
| +0x2c | `u32 format` — **0 = grey8, non-zero = RGBA (kernel converts)** |
| +0x30 | `u32[2]` zero |
| +0x38 | `u32 stride` (pixels) |
| +0x3c | `u32 height` |
| +0x40 | `u32` = 1 |

`mtk_commit` is called through the PLT, so its GOT slot is hookable — that is the shim.

## Conversion and the 4 MB copy

- `OverlayEngine::processImage(OverlayPortParam*, CommitInfo*)` copies the 1440x720
  RGBA client target (4.1 MB) into a staging buffer — **31–80 ms, avg 33.5 ms** — and
  `xrz::EffectHandler::convertToGray(src, dst, Rect, stride, bpp)` then converts it
  (args observed: `rect=(0,0,1440,720)`, `stride=5760`, source pitch = 4 bytes/px).
- `CommitInfo+0x54` is the format used both to skip the conversion and as the ioctl's
  `format`. `processImage` is PLT-called; so is `convertToGray`; both are hookable.
- `GraphicBuffer::lock/unlock` are 50–115 µs; the frame `memcmp` is 24–40 µs; the copy
  is the cost.

## Kernel e-ink nodes (`/sys/kernel/debug/eink_debug/`)

| node | meaning |
|---|---|
| `clean_a2` | kernel whole-panel fast path + ghost cleanup. **0 on the GSI at boot**, 1 on stock. Without it fast modes ghost and updates are ~2x slower. |
| `anti_flicker` | ghost/anti-flicker pass |
| `global_dither` | 0 = per mode, 5 = none (Atkinson dither mixes 1/8 of the previous frame) |
| `anti_alias` | panel-level anti-aliasing |

`clean_a2` is enabled by the stock framework on a property change
(`vendor.xrz.auto_clean_enabled=1`); the GSI has no init handler for it, so
E-ink Center writes the node directly at boot.

## Stock properties the vendor stack expects

```
ro.vendor.xrz.default_refresh_mode   178          (0xB2 Fast)
vendor.xrz.auto_clean_enabled        1
vendor.xrz.anti_flicker_enabled      1
vendor.xrz.hwc_idle                  1
vendor.xrz.disable_eink_vsync        1            (stock disables the e-ink vsync wait)
```

## Memory mapping facts

- Raw `mmap` of a gralloc dma-buf in the composer process is **uncached**: ~13 MB/s,
  80 ms for 4 MB (read or write).
- `ion_import` of the same fd fails with `ENOTTY`.
- `GraphicBufferMapper::importBuffer` + `lock` reads the buffer at ~1.8 GB/s
  (2.2 ms / 4 MB) **out of process**, but the same call inside the composer
  SIGSEGVs in `gralloc.rogue.so` (fault at 0x8, e-ink worker). See `ROADMAP-60FPS.md`.
