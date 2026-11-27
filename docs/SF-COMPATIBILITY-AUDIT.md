# SurfaceFlinger compatibility audit — HiBreak S / TD Android 16

**Date:** 2026-11-24  
**Scope:** source/binary audit, exact-branch build, and reversible on-device SF-only test. No partition was flashed; the phone was rebooted back to its original SurfaceFlinger afterward.

## Result in one paragraph

The HiBreak Pro repository is useful, but its published SurfaceFlinger patches do **not** explain a 4 MB → 1 MB reduction. Patch `0013` pre-quantizes pixels to black/white or 16-level grey while still writing into an ordinary **RGBA_8888** client target. The MT6765 vendor composer still calculates a full `stride * 4 * height` copy. The Pro series is therefore a good source for damage handling, mode policy, and GPU quality work, but it is not, by itself, the missing 60 fps optimization for this phone.

A rebased core patch is available at:

- `build/sf-audit/hibreak-s-sf-eink-core.patch`
- SHA-256: `469EB00BE2E6C3CEC12B0887D2806A786E70F76831B83DFC2DEA6A7039576DBA`

It applies cleanly to a fresh checkout of the exact TrebleDroid `frameworks/native` branch. Both the unpatched control and patched candidate compile successfully and were tested with a reversible SF-only bind mount. The phone was rebooted back to the original SF afterward.

## Exact baseline identified

The running GSI is not generic AOSP and not the HBPC LineageOS build:

| item | value |
|---|---|
| fingerprint | `google/treble_arm64_bvS/tdgsi_arm64_ab:16/BP2A.250605.031.A3/250617:userdebug/test-keys` |
| source branch | `TrebleDroid/platform_frameworks_native` → `android-16.0.0_r2-td` |
| source commit | `e134d845a8da9ec74e3ed26d2e07c1aa39b12eb4` |
| GSI SurfaceFlinger Build ID | `4feae846b0f338cbd4d2071365f937a7` |
| GSI `libgui.so` Build ID | `06fbff9cf879f30dd44ea879a16c068f` |

AOSP `android-16.0.0_r1` peels to commit `2827a4a16b0340ecd07c2d5a6c89991799b362bb`; `r1` and `r2` point to the same commit in this checkout. The TrebleDroid fork has only small deltas in the files touched by the Pro series, so it is the correct rebase target.

## Build result

Built in WSL from the exact TD branch with target `treble_arm64_bvS-bp2a-userdebug`:

| candidate | SHA-256 | Build ID |
|---|---|---|
| unpatched control | `fbc1629e17fc992015d3614617ca801318c268596260db9b739ff96dfd681351` | `e9e4645729a2e9f563ac18c162d3d485` |
| patched BW candidate | `2b34a68b7426e26ebbc5868337877b9de0ce60ec47f8bb49002f73a0f647af4e` | `3be328db72a16a500f9ac156881452e6` |

Validation:

- AArch64 PIE, stripped, 16 KiB LOAD alignment (matching the original GSI SF);
- identical `DT_NEEDED` library list;
- no graphics-common V7 dependency;
- only two new undefined symbols, both standard Bionic property APIs:
  `__system_property_area_serial` and `__system_property_serial`;
- patched binary contains the expected `EinkMotion` and `debug.sf.eink.*` strings;
- package: `build/sf-audit/hibreak-s-sf-candidates.tar.gz`
- package SHA-256: `b58049cd474e4c1d708c7f173835a0a4775124ee2f16213405db4b3bcf8e7153`

The build required `ALLOW_MISSING_DEPENDENCIES=true` for unrelated GSI test modules and a clean worktree restore after the initial interrupted sync. No SurfaceFlinger source outside the audited patch was changed for the candidate.

## Runtime result (2026-11-25)

The candidates were tested on the rooted TD GSI with a watchdog and reboot revert:

| configuration | commits / 10 s |
|---|---:|
| original GSI SF | 233 |
| rebuilt unpatched control | 235 |
| patched SF, e-ink properties off | 241 |
| patched SF, damage only | 233 |
| patched SF, BW render pass on | 211 |

With the render pass enabled, `processImage` averaged ~44 ms. Valid client-target damage reached HWC, including full `(0,0,1440,720)` and partial `(0,0,41,720)` rectangles, but the vendor still committed `(0,0,1439,719)` with `fmt=0`, `stride=1440`, and the full RGBA staging path.

The patch is therefore a working **quality/damage/mode-policy experiment**, not a 60 fps optimization. The phone was rebooted afterward; the original SF hash `a2a7a61e…` is restored and no SF/HWC overlay remains mounted.

## Pro patch compatibility

Reference: `eckelhesten/BigmeHibreakPro-LineageOS` at `c0102f8caf829546ffae78f66551bdaaa46c80d7`.

- All **22** published `frameworks/native` patches apply cleanly to current LineageOS 23.2 (`26f530caefb3217766469d9731ee602c83f48e84`).
- Only **6 of 22** apply unchanged to the exact TrebleDroid fork. The main conflicts are source drift in `SurfaceFlinger.cpp`, `Android.bp`, and the Skia include block.
- The rebased patch includes the functional core: `0002`, `0003`, `0004`, `0006`–`0009`, and `0011`–`0018`.
- Omitted:
  - `0001`: old-kernel `epoll_pwait2` fallback; unrelated to rendering and not needed for the current GSI.
  - `0010`: DPI-only.
  - `0019`–`0023`: optional reading/grey/motion refinements; add after the core is measured.

Manual rebase work included:

- adding `EinkMotion` to the older TrebleDroid `Android.bp` filegroup;
- placing the `EinkMotion` init/composite hooks in the older `SurfaceFlinger.cpp` layout;
- adding `EinkBlueNoise.h` without the newer `ShaderCache.h` include;
- placing the offscreen-composition block in the older Skia render loop;
- adapting mode numbers and defaults to HiBreak S.

## Critical correction: `0013` is prequantization, not compression

The unchanged client-target allocation is still:

```cpp
native_window_set_buffers_format(window, HAL_PIXEL_FORMAT_RGBA_8888);
```

For 720×1440, that is:

- format: RGBA_8888;
- nominal stride: 2880 bytes;
- payload: **4,147,200 bytes (~3.95 MiB)**.

The `kGray_8_SkColorType` in patch `0013` describes the 64×64 blue-noise lookup image, not the framebuffer. The final shaders output `half4(v, v, v, 1)` into the RGBA target.

The MT6765 composer disassembly confirms the consequence. In `OverlayEngine::processImage`:

- `0x9fbe0` loads height/stride;
- `0x9fc58` shifts stride left by two;
- `0x9fc80` multiplies by height;
- the resulting full `stride * 4 * height` size is used for the import/map/work path.

Client-target damage is not used to shrink that copy. Therefore the existing note saying patch `0013` makes “the composer move ~1 MB” is unsupported and should not be used as the 60 fps acceptance criterion.

Patch `0017` is still valuable: it avoids an extra Skia full-frame snapshot/COW copy by composing into an offscreen surface. It does not reduce the RGBA allocation or the vendor copy.

## HiBreak S adaptations in the rebased patch

The Pro and S mode numbers overlap but have different meanings:

| value | HiBreak S use |
|---|---|
| `183` / `0xb7` | Hyper, fastest measured motion mode |
| `16` / `0x10` | A2, 1-bit fast mode |
| `178` / `0xb2` | Fast, 16-level grey |
| `179` / `0xb3` | Extreme, not the automatic motion mode |
| `180` / `0xb4` | Regal |
| `177` / `0xb1`, `0x800000b1` | HD / HD 256 |
| `8` / `0x08` | GC4 |
| `176` / `0xb0` | CLEAN |

Changes made:

- automatic motion mode defaults to **183 / Hyper**, not Pro's `0xb3`;
- `Fast`, `Extreme`, `Regal`, `HD`, `GL16`, and `GC4` select the 16-level shader;
- sleep-clean default is **GC4 = 8**, not Pro's `4`;
- `debug.sf.eink.fast_full` defaults to `0` (cheap per-pixel FAST shader) for the first MT6765 test;
- added `debug.sf.eink.mode` as an SF-only mode override, so the first test does not depend on SurfaceFlinger being allowed to read `vendor.xrz.*`;
- retained the upstream bottom-left `native_window_set_surface_damage` conversion. An attempted top-left rewrite was rejected after runtime logging showed HWC receiving `(0,720,1440,0)`; the upstream form correctly yields `(0,0,1440,720)` and partial `(0,0,41,720)` rectangles;
- added a null-canvas fallback for the offscreen capture path;
- treated `vendor.xrz.force_global_refresh_mode=-1` (`0xffffffff`) as "no pending force"; zero remains a valid "current mode, full screen" request.

## Why the HBPC binary cannot be reused

The previous HBPC swap failed for a precise ABI reason, not merely a missing library:

- GSI `libgui.so` exports the no-argument C++ method:
  `android::SurfaceComposerClient::Transaction::setEarlyWakeupStart()`
- HBPC `libgui.so` exports a different overload:
  `setEarlyWakeupStart(android::gui::EarlyWakeupInfo)`

Those are different mangled symbols. The display binder parcels also differ. Mixing the HBPC SurfaceFlinger/libs with the TD framework is not wire-compatible.

The correct route is to build SurfaceFlinger from the **same TrebleDroid branch** with the rebased patch. Because the patch does not modify `libgui`, the first test should replace **only** `/system/bin/surfaceflinger`; do not bind-mount HBPC or LineageOS libraries over the GSI.

The Pro device tree explicitly grants `surfaceflinger` get/set access to
`vendor_xrz_prop`. That grant is not present in the stock GSI policy. Automatic
`EinkMotion` writes to `vendor.xrz.*` may therefore be denied on this phone. The
first test leaves `debug.sf.eink_motion=false` and uses `debug.sf.eink.mode=183`
instead; enable motion only after checking property access or adding an equivalent
policy/debug-property bridge.

`tools/deploy-pro-sf.sh` is not safe to use unchanged for this candidate: it labels
the binary `system_file` instead of `surfaceflinger_exec`, ignores watchdog-push
failure, and accepts any fingerprint containing `treble`. Fix those fail-closed
checks before an on-device test.

## Recommended no-flash test

1. In a TrebleDroid Android 16 tree, use `frameworks/native` branch `android-16.0.0_r2-td` at `e134d845...`.
2. Build an **unpatched** SF first with `treble_arm64_bvS-bp2a-userdebug` and `RELAX_USES_LIBRARY_CHECK=true` to validate the toolchain/product configuration.
3. Apply `hibreak-s-sf-eink-core.patch` and build `surfaceflinger` from that tree. Do not mix in HBPC/LineageOS `libgui`, `libui`, or graphics-common libraries.
4. Confirm the candidate is AArch64/PIE, has no new `DT_NEEDED` entries, does not require graphics-common V7, and still resolves the GSI's no-argument `setEarlyWakeupStart()` ABI.
5. Deploy only the SF binary with a fail-closed watchdog. The candidate must be staged under `/system/xbin` with context `u:object_r:surfaceflinger_exec:s0`; the watchdog must be verified before the bind mount/restart. Revert with `adb reboot`.
6. Set the first-test properties:

```sh
debug.sf.force_client_composition=true
debug.sf.hwc_client_target_damage=true
debug.sf.hwc_explicit_full_damage=true
debug.sf.eink_enhance=100
debug.sf.eink_render=3
debug.sf.eink.mode=183
debug.sf.eink_motion=false
debug.sf.eink.fast_full=0
debug.sf.eink.offscreen=true
debug.sf.eink.damage_pad=true
```

`debug.sf.eink_render=3` is ignored unless `debug.sf.eink_enhance` is nonzero.
The SF override selects the shader only; set `vendor.xrz.global_refresh_mode=183`
from root/E-ink Center so the vendor composer also uses Hyper.

## Acceptance measurements

Record all of the following:

- `system_server` stays alive; no default-display timeout;
- HWC shim sees non-empty client-target damage with valid `left < right` and `top < bottom`;
- client-target `fmt`, `stride`, height, and allocation size;
- `processImage` time and commit interval;
- commit rate during a continuous full-screen animation;
- visual correctness, ghosting, and tearing in Hyper and Fast modes.

Expected interpretation:

- If the target remains RGBA_8888 / stride 2880 / 4,147,200 bytes and `processImage` remains ~34 ms, the known vendor copy is still present. The patch may improve quality and damage behavior, but 60 fps should not be expected.
- If `processImage` drops below ~10 ms, inspect why before claiming success: confirm the panel is not black/stale and the client target is genuinely being consumed.
- A real 60 fps result requires either a true gray8/R8 client-target path end-to-end, or a vendor-composer path that copies/converts only damaged data.

## DSU artifact audit (no install attempted)

The old custom image is not cryptographically broken:

- raw size: 3,195,576,320 bytes;
- raw SHA-256: `e19843ca2e8256c62ee0f0717e7fb7ade7e300e4991549a94c2539d8294e8df0`;
- AVB key: `cdbb77177f731920bbe0a0f94f84d9038ae0617d`;
- FEC roots: 2;
- `e2fsck -fn`: clean.

However, `gsi/system-td-eink.img.gz` is stale:

- gzip uncompressed size: 3,170,508,800 bytes;
- current raw size: 3,195,576,320 bytes;
- difference: 25,067,520 bytes (the FEC payload);
- decompressed gzip SHA-256: `04ecc92d7f61aee83198543c86f0062b20557330398c2dcb8df5951b8cce3822`, not the raw image hash.

That mismatch is a likely cause of the earlier DSU installer/`VerificationActivity` failure because the helpers pass the current raw size while installing the older gzip. This is documentation only: **do not flash or retry the custom DSU image without explicit authorization**, and do not replace the known-good vanilla image.

## Artifacts produced

- `build/sf-audit/hibreak-s-sf-eink-core.patch` — rebased core patch.
- `build/sf-audit/apply_patch_series.py` — repeatable apply-compatibility checker.
- `build/sf-audit/*/patch-apply-results.json` — per-patch results for AOSP, LineageOS, and TrebleDroid.
- `build/sf-audit/gsi-bin/` — read-only extraction of the GSI SF/libgui/build.prop used for ABI verification.
- `refs/hibreak-pro-lineage/` — upstream reference checkout.
- `refs/treble-frameworks-native-16/` — exact TD source with the rebased patch applied on branch `audit/pro-patches`.
