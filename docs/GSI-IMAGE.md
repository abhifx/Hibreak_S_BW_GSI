# GSI image

## Known-good image (what this work was measured on)

| | |
|---|---|
| file | `system-td-arm64-vanilla.img` |
| raw size | 2.06 GB |
| gzip (for the DSU installer) | `system-td-arm64-vanilla.img.gz`, 1.01 GB |
| sha256 raw | `a0704a7909328cc077f314f25ed47c5ce02b4a095cc932dd36060e4a90d333a8` |
| sha256 gz | `d06c66e45c6d1e8d61b45d1261a65cb7ccf2e7946739602011e500a12ae55ad6` |
| build | TrebleDroid `treble_arm64_bvS`, Android 16 `BP2A.250605.031.A3`, userdebug |

Do **not** use `system-td-eink.img` from earlier experiments: it is a custom baked
image whose AVB/FEC signing path was unreliable and is not needed.

## Why it is not in this repository

GitHub rejects repository files over 100 MB and the raw image is 2.06 GB. Two options:

1. **Release asset (recommended).** Create a release on this repo and attach
   `system-td-arm64-vanilla.img.gz` (1.01 GB; the per-asset limit is 2 GB). The DSU
   installer accepts a `.gz` URI directly, so the gz is the one to publish.
2. **Bring your own GSI.** Any `arm64_bvS` Android 16 TrebleDroid build works as the
   baseline; `tools/dsu.sh` installs it as a DSU.

## Installing as a DSU (no bootloader unlock)

```bash
# on the phone: stock firmware, USB debugging on
adb devices
tools/dsu.sh install -i /path/to/system-td-arm64-vanilla.img --reboot
tools/dsu.sh status          # installed / enabled
```

The DSU keeps the stock system/vendor/boot untouched; `tools/dsu.sh disable` (on
stock) or wiping the DSU removes it. The image must pass AVB with a key the locked
bootloader accepts — that is why this specific TrebleDroid image is the known-good
one. For a self-built image, see `BUILD-GSI.md` (signing is the step that failed in
earlier hand-built attempts; verify it before relying on it).

## First boot checklist

```bash
adb root
adb shell getprop ro.build.fingerprint          # ... treble_arm64_bvS ... userdebug
adb shell getprop ro.hardware.hwcomposer        # mtk_common
adb shell ls /vendor/lib64/hw/hwcomposer.mt6765.so
```

Then apply the e-ink fix (`eink-center`) and, optionally, the shim
(`hwc-shim/deploy.ps1 -Fastconv 1`). See the top-level README.
