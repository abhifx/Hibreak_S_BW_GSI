# gray-helper tests

Standalone Android arm64 test tools for the HiBreak MT6765 helper. They do not
mount or patch SurfaceFlinger/HWC.

## Build

```powershell
pwsh -File build.ps1
```

Outputs:

- `out/gray_helper_client` — converts a synthetic RGBA pattern through a running
  helper and verifies every packed-gray byte in both 16-level and 1-bit modes.
- `out/gbfmt_dump` — captures the device's real `AHardwareBuffer` `GB01` socket
  wire format. This is how the HWC-side raw `GB01` magic, usage, descriptor and
  fd layout were verified without guessing.

## Run

Start the helper in listener mode, then run the client:

```sh
adb push out/gray_helper /data/local/tmp/gray_helper
adb push out/gray_helper_client /data/local/tmp/gray_helper_client
adb shell chmod 755 /data/local/tmp/gray_helper /data/local/tmp/gray_helper_client
adb shell "nohup /data/local/tmp/gray_helper >/data/local/tmp/hwcgray-test.log 2>&1 </dev/null &"
adb shell /data/local/tmp/gray_helper_client --iters 8
```

Dump a known-good device packet:

```sh
adb push out/gbfmt_dump /data/local/tmp/gbfmt_dump
adb shell chmod 755 /data/local/tmp/gbfmt_dump
adb shell /data/local/tmp/gbfmt_dump
```

The client reports total time including its full scalar verification pass, so
its per-frame number is higher than the helper's conversion-only timing.
