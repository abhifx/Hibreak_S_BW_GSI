# Building a system image (historical 60 fps route)

> **2026-11-25:** visible 60 fps is now achieved without a system build using the
> reversible `xproc_vendor=1` HWC/helper overlay; see `STATUS.md`. This document is
> retained for the alternative source-build route.

> **2026-11-24:** see `SF-COMPATIBILITY-AUDIT.md` for the exact source branch and a
> rebased core patch. The published Pro `0013` patch keeps RGBA_8888; it is not proof
> of a 4 MB → 1 MB optimization. Build from the TrebleDroid fork, not HBPC/LineageOS
> binaries, and measure the client-target format and `processImage` time.

Two shapes, both possible without unlocking the bootloader:

1. **Full system image** with the SF e-ink patches, installed as a DSU (recommended:
   no ABI risk).
2. **SurfaceFlinger only**, built from the same branch and bind-mounted over
   `/system/bin/surfaceflinger` (small, but the binary must match the GSI's libs).

## Host requirements

- Linux, x86_64, ~250 GB free, 16 GB RAM (32 GB comfortable).
- `repo`, OpenJDK 17+, `ccache` recommended.
- LineageOS 23.2 (Android 16) or AOSP `android-16.0.0_r*` matching the GSI build
  (`BP2A.250605.031.A3`). Using the same branch as the GSI is what keeps the HWC
  display AIDL and the framework libs ABI-compatible.

## 1. Sync and patches

```bash
mkdir -p ~/a16 && cd ~/a16
repo init -u https://github.com/LineageOS/android.git -b lineage-23.2 --git-lfs
repo sync -c -j"$(nproc)" --force-sync --no-clone-bundle --no-tags

# the Pro project's patches (Apache-2.0); apply the subset that matters:
#   frameworks/native/0013  render pass
#   frameworks/native/0004  client-target damage
#   frameworks/native/0002  explicit full damage
#   frameworks/native/0006  motion fast (optional)
cd frameworks/native
git am /path/to/BigmeHibreakPro-LineageOS/patches/frameworks/native/0013-*.patch
git am /path/to/BigmeHibreakPro-LineageOS/patches/frameworks/native/0004-*.patch
git am /path/to/BigmeHibreakPro-LineageOS/patches/frameworks/native/0002-*.patch
```

Expect fuzz or rejects on a different branch; the render pass hunks are the large
ones. Keep the `debug.sf.eink_*` properties from the Pro `system.prop` — the pass is
driven by them.

Adapt for this panel:

- resolution 720x1440 (the pass works on the composed frame, so mostly it is just the
  properties); set `debug.sf.eink.*` values for a mono 16-level panel;
- the vendor composer is `mt6765`, but the patches are generic SF code, not
  device-specific;
- do not copy the Pro's **mode numbers** (`0xB0/0xB3`): this library uses
  `0xb1` HD, `0xb2` Fast, `0xb3` Extreme, `0x11` Hyper, `0x10` A2.

## 2. Build a GSI-compatible system image

Use a Treble/GSI target so the image can be installed as a DSU (a `treble_arm64_bvS`-
style lunch target from the TrebleDroid manifests works; a plain `aosp_arm64` target
also produces a Treble system image):

```bash
source build/envsetup.sh
lunch treble_arm64_bvS-userdebug      # or the GSI target from your manifest
make -j"$(nproc)" systemimage
# output: $OUT/system.img
```

## 3. Sign it for the DSU

The bootloader is locked. A DSU image must pass AVB with a key the stock
first-stage accepts; that is why the stock TD image works. Options:

- sign with the same key the known-good TD image uses (see
  `tools/inspect-avb-keys.sh` in the full project repo), or
- use the re-sign helper (`tools/resign-gsi.sh`) on your `system.img`.

This step is the one that failed in earlier attempts with hand-built images
(FEC/re-signing). Plan for it: verify with the DSU installer, and keep the known-good
`system-td-arm64-vanilla.img` untouched as the fallback.

## 4. Install and test

```bash
# from the full project repo (DSU helper)
tools/dsu.sh install -i /path/to/system.img --reboot
tools/dsu.sh status
```

After boot:

```bash
adb root
adb shell getprop debug.sf.eink_render      # 3 once the pass is in use
adb shell dumpsys SurfaceFlinger | head -40
adb shell 'sh /data/local/tmp/measure.sh 8' # commits/s + app frames
```

Acceptance: client-target damage non-empty in the shim log, `processImage` well under
10 ms, scrolling at ≥50 commits/s with a correct image. See `ROADMAP-60FPS.md`.

## Route 2 — SurfaceFlinger only

If a full image is too much, build just SF from the same tree:

```bash
make -j"$(nproc)" surfaceflinger
# $OUT/system/bin/surfaceflinger (+ any libs it links that differ)
```

and deploy it with `tools/deploy-pro-sf.sh` (pushes, bind-mounts over
`/system/bin/surfaceflinger`, arms a watchdog, restarts SF then zygote). Revert with
`-Revert` or `adb reboot`. Check `tools/extract-pro-sf.sh` to test the Pro's published
binary first — it tells you whether the ABI is close before you build anything.

**Risk:** a mismatched SF takes the UI down (recoverable by reboot). Do not do this on
a production phone without the watchdog.
