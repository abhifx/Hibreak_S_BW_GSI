# gray_helper

Standalone Android arm64 helper for the HiBreak S MT6765 HWC experiment. It
receives a SurfaceFlinger-style `AHardwareBuffer` over a Unix
`SOCK_SEQPACKET` socket, initializes EGL/PowerVR in its own process, converts
RGBA8888 to packed 8-bit gray with GLES2 (NEON fallback), and sends an RGBA8888
backing `AHardwareBuffer` handle containing that gray data.

It does **not** load the HWC shim, modify SurfaceFlinger, mount anything, or
write to a persistent partition.

## Build

Windows:

```powershell
pwsh -File build.ps1
# optional: pwsh -File build.ps1 -Ndk C:\Android\ndk -Api 30
```

Linux/macOS:

```sh
./build.sh
# optional: NDK=/path/to/android-ndk API=30 ./build.sh
```

The scripts target `aarch64-linux-android30`, disable exceptions/RTTI, and use
`-nostdlib++`. Output is `out/gray_helper`; required libraries are EGL, GLESv2,
nativewindow, android, and log. There must be no `libc++`/`libc++_shared` entry
in `readelf -d`.

## Run

```sh
adb push out/gray_helper /data/local/tmp/gray_helper
adb shell chmod 755 /data/local/tmp/gray_helper

# One connection, useful for protocol tests:
adb shell /data/local/tmp/gray_helper --once

# Daemon (default socket /data/local/tmp/hwcgray.sock):
adb shell 'nohup /data/local/tmp/gray_helper >/data/local/tmp/hwcgray.log 2>&1 &'

# Reverse mode: HWC binds and listens on abstract name hwcgray_hwc;
# the root helper connects, retrying every 50 ms until SIGTERM:
adb shell 'nohup /data/local/tmp/gray_helper --connect hwcgray_hwc >/data/local/tmp/hwcgray.log 2>&1 &'

# Reverse mode, exactly one connection:
adb shell /data/local/tmp/gray_helper --once --connect hwcgray_hwc

# Force the CPU NEON converter for comparison or fallback testing:
adb shell /data/local/tmp/gray_helper --cpu
```

`--socket PATH` selects another filesystem listener. `--connect NAME` instead
makes the helper an outbound client of a Linux abstract AF_UNIX listener; NAME
has no leading NUL or filesystem path, and the helper prepends the abstract
NUL internally. Connect mode creates no filesystem socket. After each successful
connection it processes exactly one request, closes, and reconnects; `--once`
exits after that one connection in either mode. `--cpu` forces the NEON path;
otherwise GPU conversion is attempted first with automatic NEON fallback.
`--help` prints usage.

Filesystem listener sockets are created mode `0666`; Android SELinux can still
deny access. This is a standalone helper, not an init service. The HWC side owns
the abstract listener in reverse mode and performs the same accept/send/receive
steps described below.

## Wire protocol

All integers are little-endian. Both fixed packets are exactly 24 bytes and use
`#pragma pack(push, 1)` equivalent layout.

```c
struct GrayRequest {
    uint32_t magic;       // 0x31475748, bytes "HWG1"
    uint32_t version;     // 1
    uint32_t width;
    uint32_t height;
    uint32_t onebit;      // 0 = 16-level, 1 = black/white
    uint32_t reserved;    // 0 = no raw target, 1 = raw target packet follows
};

struct GrayError {
    uint32_t magic;       // 0x31455748, bytes "HWE1"
    uint32_t version;     // 1
    int32_t  status;
    int32_t  native_error;
    uint32_t reserved[2]; // 0
};
```

One connection carries exactly one request in either listener or `--connect`
mode. In reverse mode, HWC is the socket peer and the helper dials out, but the
packet and handle directions are unchanged:

1. Client sends one `GrayRequest` packet.
2. Client sends one input handle with
   `AHardwareBuffer_sendHandleToUnixSocket()`.
3. If `reserved == 1`, client sends one additional SOCK_SEQPACKET packet with
   payload `uint32_t 0x314f5748` (bytes `HWO1`) and exactly one SCM_RIGHTS fd.
   The fd is the raw packed-gray output target. `reserved == 0` sends no such
   packet and keeps the original behavior.
4. After conversion, `reserved == 1` makes the helper mmap the raw fd
   `PROT_READ|PROT_WRITE MAP_SHARED`, copy exactly `width * height` bytes from
   offset 0 of the packed RGBA backing, attempt `msync(MS_SYNC)`, unmap, and
   close the received fd. The target must be at least that large. This kernel
   rejects `msync` for dma-bufs, so that failure is logged and treated as
   non-fatal, matching the vendor's own MAP_SHARED behavior.
5. On success, helper still sends the normal output AHardwareBuffer handle with
   `AHardwareBuffer_sendHandleToUnixSocket()`. There is no success header; HWC
   may use this handle as completion/ack after the raw target is populated.
6. On failure, helper sends one `GrayError` packet with no SCM_RIGHTS, then
   closes the connection.

A client that needs error details should inspect the reply with `recvmsg()` or
`recv(MSG_PEEK)` before calling `AHardwareBuffer_recvHandleFromUnixSocket()`:
if it begins with `HWE1`, consume the fixed error packet; otherwise consume the
AHardwareBuffer rights packet.

Input must be single-layer `AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM` with exactly
the requested dimensions and a CPU-readable allocation. The producer must make
its writes complete before sending the handle.

Output is a single-layer `AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM` backing buffer
with the same dimensions, but its logical payload is packed gray8: exactly
`width * height` bytes at mapping offset 0, with logical pitch `width`. The
remaining RGBA backing bytes are not part of the gray image. Clients commit the
returned fd with e-ink `format=0`, `stride=width`, `height=height`; they must not
use the RGBA descriptor's `stride * 4` as the gray pitch.

### Error status values

| Value | Meaning |
|---:|---|
| 1 | malformed request/protocol |
| 2 | socket I/O failure |
| 3 | EGL/PowerVR initialization failure |
| 4 | output pool allocation failure |
| 5 | input handle import failure |
| 6 | unsupported/mismatched input buffer |
| 7 | daemon pool geometry differs from request |
| 8 | input lock failure |
| 9 | output lock failure |
| 10 | input unlock failure |
| 11 | output unlock failure |
| 12 | output handle send failure (an error packet may not be deliverable) |
| 13 | raw target packet/fd missing, malformed, or too small |
| 14 | raw target mmap failure |
| 15 | raw target copy validation failure |
| 16 | reserved (msync failure is non-fatal) |
| 17 | raw target munmap failure |

`native_error` preserves the AHardwareBuffer return value, `errno`, or EGL
error code from the failing stage.

## Conversion and pool lifetime

- BT.601 integer luma: `(77*R + 150*G + 29*B) >> 8`.
- `onebit=0`: 4x4 Bayer values 0..15, `q = min((luma + bayer) >> 4, 15)`,
  output `q * 17`.
- `onebit=1`: 8x8 Bayer values 0..63, output is 255 when
  `luma + bayer >= 128`, otherwise 0.
- Input rows use `input_desc.stride * 4` bytes. Output is one contiguous packed
  gray plane: row `y` starts at `y * width`, for exactly `width * height` bytes
  at offset 0. RGBA row padding and the rest of the backing allocation are not
  used as gray output.
- EGL is initialized lazily once. The current ES2 context brings up the PowerVR
  client for both GPU conversion and the cached CPU-lock fallback.
- By default, input and output AHardwareBuffers are imported as EGL images and
  GLES2 textures. The output texture is attached to an FBO. Each fragment
  samples four source texels, computes four BT.601 gray values with the selected
  Bayer threshold, and packs them into R,G,B,A. No CPU lock or read of the input
  occurs on this path. `glFinish()` completes GPU work before the handle is sent.
- The logical GPU target is `width/4 x height` RGBA fragments, producing exactly
  `width * height` packed bytes. GPU conversion requires `width % 4 == 0` and
  an unpadded backing row (`AHardwareBuffer_Desc::stride == width`). The FBO
  renders the dense physical prefix and the fragment shader maps it to the
  packed payload; padded-stride geometries use NEON because row-padding pixels
  are not addressable through the EGLImage texture.
- Per-request EGL images, textures, and FBOs are destroyed. The shader program,
  Bayer textures, and quad VBO are cached. Any GPU setup/draw failure falls back
  to the existing CPU NEON converter; `--cpu` skips GPU setup entirely.
- Four RGBA8888 backing buffers are allocated with
  `CPU_READ_RARELY | CPU_WRITE_RARELY | GPU_FRAMEBUFFER` first, then with
  `GPU_SAMPLED_IMAGE` added if needed. CPU-only allocation remains the final
  compatibility fallback. The helper validates format, layers, dimensions,
  stride, required usage, and size arithmetic proving the backing allocation
  can hold the packed `width * height`-byte payload.
- With `reserved == 1`, the raw target receives the same packed prefix before
  the normal output handle is returned. The received raw fd is always closed by
  the helper; input AHardwareBuffers are released on every success/error path.
- The helper retains a reference to every pool entry and rotates through them.
  A client must release each received output after consuming it and must not
  have more than four output handles outstanding, or an older handle may be
  overwritten.
- Geometry is fixed for the daemon lifetime. Restart it to change width/height;
  a mismatched request receives status 7.
- `--once` accepts one connection and exits after that request, successful or
  not.

### Why the backing is RGBA8888

The tested TD A16 `gralloc.rogue` rejects `AHARDWAREBUFFER_FORMAT_R8_UNORM`, but
the vendor's cached RGBA8888 commit buffer can be read by the e-ink path as gray
format 0. The helper therefore stores packed gray in the first
`width * height` bytes of an RGBA allocation and returns that handle. This keeps
the wire protocol unchanged while matching the vendor commit layout.

## Safety

Keep the helper and socket under `/data/local/tmp`. Stop it with SIGTERM before
removing the socket or binary. It does not integrate with the composer yet;
connecting an HWC process may require an SELinux rule or a differently labelled
socket. Do not use this helper as a reason to enable `skipcopy`: on this GSI that
path commits black frames. No kernel, bootloader, DSU, or partition changes are
part of this tool.
