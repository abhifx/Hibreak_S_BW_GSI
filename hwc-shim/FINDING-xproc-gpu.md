# Cross-process gray converter for the MT6765 GSI

**2026-11-26 — race/lifetime hardening and live validation.**

The final design removes the wrong-frame source guess, the destination-fd
guess, and the `cfg->fd` rewrite:

- `setClientTarget` clones the candidate and its acquire fence before the vendor
  consumes them, then publishes the snapshot only if the real HWC call succeeds.
  Snapshots are keyed by dma-buf `(st_dev, st_ino)`, not by fd number.
- `processImage` binds a snapshot at job start, preferring the fd at
  `OverlayPortParam+0x78`; if the vendor path carries a layer/staging fd there,
  it falls back to the latest accepted CT snapshot at job start (not conversion),
  so a newer `setClientTarget` cannot swap it mid-job.
- The exact vendor `CommitBuffer+0x18` fd is captured under `OverlayEngine+0x428`
  at job start. The helper writes that fd; the physical ioctl still runs inside
  the vendor's normal `triggerEinkCommit` path with its own unmodified fd.
- If the helper fails after the 4 MB staging copy was skipped, the shim converts
  the same bound snapshot into that same vendor buffer. If even that fails, the
  next commit is dropped rather than publishing partial/stale data.
- A timed-out request quarantines the destination for 8 s and opens a circuit
  breaker after three consecutive failures; while the circuit is open, the
  staging copy is not skipped. Sustained failure sets
  `vendor.debug.hwc.shim.helper_recovery=1`; `helper-watchdog.sh` kills and
  restarts the tracked helper.
- The helper prewarms EGL/gralloc, rebuilds its output pool after a geometry
  change, rejects real `msync` failures (tolerating only
  `ENOTTY/EINVAL/ENOSYS`), and fails closed if socket timeout setup fails.

**Earlier fd-redirection build (do not redeploy):** `hook_mtk_commit` rewrote
`cfg->fd` to a helper-filled vendor buffer. That build is obsolete. The 1.3 GB
`SUnreclaim` event was subsequently traced primarily to stale empty per-PID
cgroup2 directories, but the fd rewrite is still removed by design.

Final live validation on 2026-11-26 (persistent TD DSU, clean boot):

- Fast 10 s: **581 physical commits (58.1/s)**, 120 app callbacks/s, zero
  fallbacks, quarantines, or crashes.
- Hyper 60 s: **3523 physical commits (58.72/s)**, 116–122 app callbacks/s, zero
  fallbacks, quarantines, or crashes.
- During the 60 s soak, `MemAvailable` stayed ~2.06–2.08 GB, `SUnreclaim` ~343 MB,
  composer RSS 23,324 kB, helper RSS 56,496 kB, helper fds 44–49, composer fds
  204–220, and dma-buf counts remained bounded.
- Helper kill/recovery replaced the helper in 4 s without changing the composer;
  the next Hyper 10 s run reached 588 commits (58.8/s), zero fallbacks.
- HD 256 and Regal produced 41 and 40 vendor commits in 6 s with zero helper
  conversions. Screen-off/wake kept all four display PIDs; the final post-wake
  Hyper 10 s run reached 595 commits (59.5/s), zero fallbacks.
- Final shim SHA-256 `9b72b05e...79814`; helper `e7596065...55278`.

Deployment defaults now match the known-good profile; unsafe
`Fast=1`+`Xproc`, `Skipcopy=1`, and quality-mode helper combinations are
rejected. Vendor ABI hashes are enforced, SELinux label copy/readback failures
are fatal, and measurement/deployment property state is restored.

**2026-11-25 — visible 60 fps achieved in Fast mode with the vendor-pipeline helper.**

The working configuration is `-Fast 0 -Xproc 1 -XprocVendor 1`. The GPU helper
writes packed gray directly into the vendor's own gray commit dma-buf(s), while
the vendor worker performs the normal physical commit. The user confirmed a
moving image, responsive touch, and ghost clearing.

## Status (2026-11-26): 60 fps vendor path, exact destination, no fd rewrite

The supported overlay is still `-Fast 0 -Xproc 1 -XprocVendor 1 -Mode -1`.
It keeps `mtk_commit` and the vendor worker/scan-out semantics, but the helper
writes into the **exact vendor `CommitBuffer` dma-buf** selected for the current
`processImage` job. `hook_mtk_commit` no longer rewrites `cfg->fd`.

The shim does not guess the destination from pool state. During the active
`processImage` job, the vendor locks its selected output with
`GraphicBuffer::lock(usage=0x30)`. `hook_gb_lock` matches that exact `GraphicBuffer`
object against the `CommitBuffer+0x8` slots at `OverlayEngine+0x610/+0x618/+0x620`
under `OverlayEngine+0x428`, then duplicates the matching `CommitBuffer+0x18` fd.
The helper writes that fd; `triggerEinkCommit` and `mtk_commit` continue with the
vendor's own unmodified fd. No round-robin guess, no `cfg->fd` substitution, and
no separately allocated helper-buffer physical commit. A timed-out destination is
quarantined by dma-buf identity for 8 s, forcing the vendor converter until safe.

The helper uses a persistent `SOCK_SEQPACKET` connection and pre-created output
EGLImages/textures/FBOs. Input imports/releases and all output GL resources are
freed on failure. The helper returns one four-slot RGBA-backed packed-gray pool
entry per frame; its backing is rotated and remains gralloc/GPU-owned.

### Correcting the slab diagnosis

The earlier 1.3 GB `SUnreclaim` reading was **not a helper/GPU buffer leak**.
This kernel exposes empty Android per-PID cgroup2 directories such as
`/sys/fs/cgroup/system/uid_1000/pid_17558`. Removing those empty directories
dropped `SUnreclaim` from **1.348 GB to 371 MB** in the same boot, without a
reboot, and free memory recovered by roughly 1 GB. 1,011 stale cgroups were
removed; a small live set was left untouched.

`device/helper-watchdog.sh` and E-ink Center now remove only empty per-PID
cgroup directories. They never remove populated process groups. E-ink Center
also runs this on screen on/off, during input pulses (throttled), at boot, and
during provisioning. `Root.run` now uses the existing persistent root shell
instead of spawning a new `su` + root shell for every action.

Current code also fixes a real fallback leak in `ct_map`: retained ION imports
and their returned map fds are now released on eviction/error instead of being
leaked when the CT rotates.

### Final measured after cleanup (this session)

- Fast 10 s: **581 commits (58.1/s)**, zero fallbacks.
- Hyper 60 s: **3523 commits (58.72/s)**, zero fallbacks.
- Hyper 10 s after helper recovery: **588 commits (58.8/s)**, zero fallbacks.
- Hyper 10 s after screen-off/wake: **595 commits (59.5/s)**, zero fallbacks.
- Composer/helper PIDs and RSS were stable; helper fds stayed 44–49 and composer
  fds 204–220. `SUnreclaim` stayed ~343 MB and `MemAvailable` ~2.06 GB through
  the 60 s run. Earlier overlapping-run counts, including ~9500/60 s and
  5320/60 s, were invalid and are discarded.

### Validation completed

1. Exact-output `GraphicBuffer` matching confirmed in live logs.
2. Clean 60 s Hyper soak completed with flat memory/fd/RSS/dma-buf samples.
3. Helper kill/recovery passed without a composer restart.
4. HD 256 and Regal bypassed the helper and kept the composer alive.
5. Persistent DSU reboot verified; the runtime HWC overlay was redeployed after
   reboot and the final post-wake smoke test passed.

### Latest runtime fixes

- `CtMap` slots initialize `ion_handle=-1`/`map_fd=-1`; retained ION imports and
  map fds are freed on eviction/error.
- The exact destination is matched from the vendor output `GraphicBuffer` during
  `lock(usage=0x30)`, not guessed from rotating pool pointers.
- Empty-cgroup cleanup and the clean DSU boot keep `SUnreclaim` around 343 MB
  rather than the earlier 1.3 GB event.
- Helper kill/recovery passed in 4 s with the composer PID unchanged.
- E-ink Center navigation is one transparent 28 dp (48 px) non-touchable gesture
  strip with an 8 dp handle. Static overlay ownership prevents duplicate strips
  across accessibility reconnects; three reconnect cycles retained one window.
- E-ink Center full refresh restores the previous refresh mode after GC16.

## Perfetto smoothness tuning (2026-11-27)

The panel path was already at 60 Hz, but tracing showed full-frame client
composition and gralloc import overhead consumed the frame budget. The persisted
profile now disables SurfaceFlinger luma/RegionSampling (`debug.sf.luma_sampling=0`)
because brightness is manual, and the helper caches four imported client targets
by dma-buf identity. This removed repeated import/free traces and reduced average
helper conversion from ~13.5 ms to ~11.1 ms.

Warm Settings scrolling improved to 7.64% jank with 21 ms median / 30 ms 90th /
34 ms 95th / 44 ms 99th. Final Hyper 60 s: 3593 commits (59.88/s), zero fallbacks,
flat `SUnreclaim` ~281 MB, stable RSS/PIDs, and bounded fds/dma-bufs. Helper
recovery and reboot persistence passed; post-reboot Hyper measured 598/10 s
(59.8/s).

## Earlier architecture

Remove the vendor composer's uncached 4 MB RGBA staging copy without replacing
SurfaceFlinger or flashing a system image, while preserving the vendor's proven
physical e-ink commit path.

## Working architecture

1. HWC snapshots the SurfaceFlinger client target and its acquire fence.
2. HWC sends a raw `GB01` GraphicBuffer packet plus `SCM_RIGHTS` to the helper.
3. The helper imports the client target in its own process and converts it with
   GLES2/PowerVR. Four gray bytes are packed into each RGBA output fragment.
4. HWC sends one of the vendor's captured gray commit fds in an `HWO1` packet.
5. The helper copies the packed gray into that vendor dma-buf.
6. The vendor's normal `processImage` → `triggerEinkCommit` → `mtk_commit` path
   commits the buffer. Mode policy, fences, scan-out, touch, and ghost cleaning
   remain vendor-controlled.

An abstract Unix `SOCK_SEQPACKET` socket (`hwcgray_hwc`) works with SELinux
**enforcing**. HWC listens; the root helper keeps a persistent connection and
handles successive frames until it closes. No filesystem socket or policy
relaxation is needed.

## Measured results

Fast mode (`178 / 0xb2`), full-panel animation:

| Test | Physical vendor commits | App callbacks | Fallbacks |
|---|---:|---:|---:|
| 10 s | 623 (62.3/s) | 114–122/s | 0 |
| 10 s repeat | 644 (64/s) | 120–122/s | 0 |
| 30 s sustained | 1886 (62.9/s) | 120–124/s | 0 |
| 10 s repeat | 639 (63.9/s) | 120–122/s | 0 |

Hyper (`183 / 0xb7`) with 16-level gray: **633 commits/10 s (63.3/s)**,
Choreographer 120–122/s, zero fallbacks. The user confirmed a visible grayscale
gradient in Hyper.

Follow-up hardening after the E-ink Center UI work found and fixed a
`native_handle_clone` use-after-free: the vendor worker now clones the `GB01`
native handle while holding the snapshot lock, so a newly installed client target
cannot free the metadata/fds during conversion. No client-target or acquire-fence
fd ownership was changed. The fixed build measured **610 commits/10 s (61/s)** and
**1851 commits/30 s (61.7/s)** in Hyper, 104–120 app callbacks/s, zero fallbacks,
and no composer restart or new crash.

A later stress run exposed a crash in the profiling-only `memcmp` GOT hook. That
hook is removed from the production build. The current build measured 602/10 s
and 1794/30 s (~59.8/s) in Hyper with a stable composer PID and no crash; six
frames used the safe vendor-converter fallback.

HD 256 then exposed a separate vendor `processImage` crash when the helper's
staging-copy skip was active. The shim now reads the live refresh mode and uses
the helper only for Fast/Extreme/Hyper/A2. HD 256 and Regal always use the full
vendor conversion/copy path. HD 256 passed 20 repeated shade open/close cycles
with an unchanged composer PID; Hyper measured 602/10 s with zero fallbacks.

A helper-process death exposed one more fail-safe issue: the shim kept skipping
the vendor staging copy after conversion failures, so the fallback converter
could read stale data and leave the panel frozen. Any helper failure now clears
`g_xproc_ready`, immediately restoring the full vendor copy on the next frame.
`helper-watchdog.sh` restarts the helper within five seconds while `xproc=1`.
A controlled prolonged-outage test confirmed the vendor copy resumed and the
composer stayed alive; after restoration Hyper measured 593/10 s with one safe
fallback.

A recurring vendor `processImage` crash then showed a 4 MB `memcmp` called with
the sentinel pointer `-1`. The shim now routes `memcmp` through a guarded hook:
only clearly invalid large-buffer pointers are rejected; normal comparisons go
directly to libc. A 60 s Hyper soak measured 3420 physical commits (57/s), zero
fallbacks, no guarded-pointer event, an unchanged composer PID, and no crash.
HD 256 also passed 20 repeated shade cycles on the same composer.

The 30 s count uses a 16 MB logcat buffer; the default buffer wraps under verbose
per-frame logging and undercounts long runs. Helper conversion plus raw copy is
~10.4 ms average; vendor `mtk_commit` is ~3.0–4.0 ms. Helper RSS is stable around
56.6 MB. A previous input-handle ownership leak was found and fixed before the
sustained run.

## Rejected direct-commit mode

`-Fast 1 -Xproc 1` commits the helper's separately allocated RGBA-backed buffer
directly through `mtk_commit`. It produces 61–62 successful software calls/s and
correct screenshots, but the physical panel stays frozen and touch becomes
unresponsive. This mode is **invalid** and must not be used.

Evidence:

- Vendor and direct configs have identical fields, including zero reserved words
  and `flag=1`.
- Vendor gray fd: `mtk_commit` ~2.7 ms and visible refresh.
- Helper allocation: `mtk_commit` ~34–38 µs and no physical refresh.
- Dma-buf sync ioctl and `msync` are unsupported (`ENOTTY` / `EINVAL`).

The working mode therefore keeps the vendor's buffer and commit pipeline.

## Converter details

- MT6765 gralloc rejects `R8_UNORM`; the helper uses an RGBA8888 backing pool
  (`usage 0x222`) and treats its first `width * height` bytes as packed gray8.
- The real GSI client target is uncached for CPU reads (~78–80 ms), so the GPU
  path performs no CPU input read. NEON remains an automatic fallback.
- Synthetic GPU verification passed 160/160 measured frames byte-exact.
- Known-good device `GB01` wire format was captured with
  `gray-helper-test/gbfmt_dump.cpp`: magic `0x47423031`, usage `0x322`, three fds
  and 47 metadata ints for 1440×720 RGBA.
- Raw vendor-output copy uses `HWO1` (`0x314f5748`) plus one `SCM_RIGHTS` fd.
  `msync` failure is non-fatal because this dma-buf/kernel rejects it and the
  vendor uses the same `MAP_SHARED` mapping without `msync`.
- The vendor conversion path keeps a locked clone of the client target's native
  handle for each request. `g_snap` still owns the original; the lease prevents
  the next `setClientTarget` from invalidating an in-flight `GB01` send.

## Reversible deployment

Build:

```powershell
pwsh -File build/hwc-shim-mt6765/build-all.ps1
```

Deploy the working Fast-mode configuration:

```powershell
pwsh -File build/hwc-shim-mt6765/deploy.ps1 `
  -Fast 0 -Xproc 1 -XprocVendor 1 -HelperOnebit 0 -Mode -1 `
  -Nofence 0 -EinkVsync 1 -Log 1 -LogEvery 60 -Watchdog 90
```

Measure physical vendor commits and app callbacks:

```sh
adb push tools/measure-xproc-vendor.sh /data/local/tmp/measure-xproc-vendor.sh
adb shell chmod 755 /data/local/tmp/measure-xproc-vendor.sh
adb shell sh /data/local/tmp/measure-xproc-vendor.sh 10 178 1
# Hyper, forcing 0xb7 while retaining 16-level gray:
adb shell sh /data/local/tmp/measure-xproc-vendor.sh 60 183 1
```

Revert in the same boot:

```powershell
pwsh -File build/hwc-shim-mt6765/deploy.ps1 -Revert
```

`-Revert` writes `persist.disabled`, detaches the bind mount, and keeps the vendor
composer active after reboot. With persistence enabled, reboot reapplies the same
verified profile through E-ink Center's boot receiver. No partition, DSU, kernel,
waveform, or SurfaceFlinger binary is modified.

## Stop conditions

Stop and run `deploy.ps1 -Revert` if the image is black/stale, tears, the framework
crash-loops, or the watchdog fires. Reboot alone reapplies a persistence-enabled
profile. The first helper frame uses the vendor copy as a safe baseline; later
helper failures restore the vendor converter. Do not use `skipcopy` by itself,
which can commit black frames.
