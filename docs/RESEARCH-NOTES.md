# HiBreak Android 16 GSI / DSU notes

## Device

| Property | Value |
|---|---|
| Model | Bigme HiBreak (`HiBreak` / `Smartphone`) |
| SoC | MT6765 (arm64, A/B, current slot `_b`) |
| Stock build | Android 14 / SDK 34, `user`, `dev-keys`, SELinux enforcing |
| Vendor | `ro.vendor.api_level=30`, `ro.vndk.version=31` (Android 11 vendor on Android 14 framework) |
| Bootloader | locked, verified boot `green`, `sys.oem_unlock_allowed=0`, `ro.oem_unlock_supported` unset |
| DSU | `com.android.dynsystem`, `dynamic_system` service, `gsi_tool` present |

## Status 2026-11-18 — route B boots green; DSU armed for a one-shot test

Stock is intact and booting: slot `_b`, `verifiedbootstate=green`,
`flash.locked=1`, `veritymode=enforcing`.

On the third attempt the re-signed chain was accepted: the phone booted stock
Android with `boot_b.new.img` + `vbmeta_b.new.img` and stayed **green, locked,
enforcing**. That proves the AOSP AVB test key (`cdbb77…`) is the bootloader's
root of trust here, and the vbmeta re-chain is legitimate.

The TrebleDroid image was then installed as a DSU (2.2 GB system + 8 GB user
data, `/data` free dropped 24.2 → 14 GB, installer `onPostExecute result: 1`),
and `gsi_tool enable --single-boot` succeeded. The phone is booting the GSI for
the first time.

Earlier failures on the way here, both without any partition damage:

1. First flash: write succeeded, but `mtk.py w` leaves the DA resident — the
   phone looked dead until a physical power cycle. Fixed with
   `mtk.py multi "w …;reset"`.
2. Second flash: a stale `.state` from the previous session made `connect()`
   call `reinit()` and skip the DA upload on a fresh connection
   (`Wrong magic`, empty GPT). Fixed by deleting `.state` before each session.

Still pending: `tools/flash-route-b.sh readback` (BROM readback of both
partitions, compares to the build artifacts).

```bash
tools/flash-route-b.sh verify     # full offline check, writes nothing
tools/flash-route-b.sh flash      # boot_b + vbmeta_b, then boot to stock
tools/flash-route-b.sh readback   # re-read both over BROM and compare
tools/dsu.sh install --single-boot # then test the GSI
```

## DSU install recipe (works)

`dsu.ps1` drives `com.android.dynsystem` via `START_INSTALL`. `gsi_tool install`
itself refuses non-root on this `user` build; the shell uid holds
`INSTALL_DYNAMIC_SYSTEM`, so the intent path works.

Three non-obvious requirements:

1. The DSU installer only accepts `.gz` / `.gzip` / `.zip` URIs. A raw `.img`
   fails with `UnsupportedFormatException`. Use gzip (it is streamed and
   decompressed on-device).
2. `KEY_SYSTEM_SIZE` must be the **exact** raw image size. Padding after the
   image moves the AVB footer away from the partition end and `gsid` reports
   `invalid AVB footer`.
3. A completed intent install can land in `disabled`; run `gsi_tool enable`
   (optionally `--single-boot`).

```powershell
.\dsu.ps1 -Action Status
.\dsu.ps1 -Action Install -SingleBoot          # uses gsi\system-td-arm64-vanilla.img.xz
.\dsu.ps1 -Action Install -ImageXz gsi\system-testkey.img -SkipHashCheck -SingleBoot
.\dsu.ps1 -Action Disable                      # stay on stock
.\dsu.ps1 -Action Wipe                         # reclaim space (run from stock)
```

`-SingleBoot` writes `disabled` before booting, so a failed attempt falls back
to stock on the next power cycle.

## Blocker: DSU cannot boot non-Bigme images on the locked bootloader

> Historical analysis (2026-11-14). The conclusion "only a Bigme-signed image
> can boot" was wrong: the top-level vbmeta is signed with the AOSP AVB test
> key, and the stock ramdisk simply lacks `/avb`. See "Route B" below.

Both attempts reset the SoC before Android starts:

```
USB 2000 (BROM) -> reset -> 2000 -> reset -> 201C (preloader, steady)
```

Tried:

- TrebleDroid Android 16 `system-td-arm64-vanilla` (signed with key `cdbb7717…617d`)
- Same image re-signed with the AOSP test key (`de54aa1a…7b1d`) via
  `avbtool erase_footer` + `add_hashtree_footer`

Both fail identically. **This does not prove the trust anchor is neither key** —
see "Corrections" below: the test-key image was malformed.

Why: `fs_mgr`'s `TransformFstabForDsu()` mounts DSU partitions with
`avb_keys=/metadata/gsi/dsu/avb` — the device's own trusted keys copied from the
ramdisk `/avb`. First-stage mount verifies the DSU image against those keys, so
only an image signed by Bigme can boot. Install-time checks are permissive for
local `file://` URIs, which is why the install succeeds but boot does not.

No usable boot logs on this build: `/sys/fs/pstore` is SELinux-blocked for
`shell`, there is no `last_kmsg`, and the build is `user` (no `adb root`, no
`su`).

## Corrections and open questions (2026-11-14)

> Kept for the source-code trail. Question 3 ("which keys are in the stock
> ramdisk `/avb`?") is answered: **none** — the directory does not exist.
> Route B installs the needed key itself.

### 1. The test-key image was malformed — the old conclusion is invalid

`gsi/system-testkey.img` (the old one) was signed with an **empty hashtree
`Partition Name`** and no FEC:

```
Hashtree descriptor:
  Partition Name:        <empty>      <-- fatal
  FEC num roots: 0
```

`TransformFstabForDsu()` sets `logical_partition_name = "system"` and
`GetHashtreeDescriptor()` matches descriptors by exact name (length + bytes), so
that image could never set up dm-verity on a DSU `/system`, *whatever* key it
carried. The "re-signed with the AOSP test key and it still failed" result is
therefore not evidence about the device's key.

Re-signed correctly with `tools/resign-gsi.sh`
(`--partition_name system`, SHA256_RSA2048, no FEC because the `fec` binary is
absent). New `gsi/system-testkey.img`:

| Field | Value |
|---|---|
| size | 2193584128 bytes |
| sha256 | `af782a0ca99c73620c815c845eb657ea35c9d3e339e99f74a28f3a9938a8bb02` |
| public key sha1 | `de54aa1af52911063fc680e6cc1edaf3aa4d7b1d` |
| partition name | `system` |
| rollback index | 0, location 0, flags 0 |

### 2. The blocker mechanism, from AOSP source

`system/core/fs_mgr/libfstab/fstab.cpp`:

```cpp
void TransformFstabForDsu(...) {
    static constexpr char kDsuKeysDir[] = "/avb";   // stock ramdisk
    ...
    entry->avb_keys = kDsuKeysDir;
```

`system/core/fs_mgr/libfs_avb/fs_avb.cpp`:

```cpp
static bool IsAvbPermissive() {              // false when locked
    if (IsDeviceUnlocked()) { ... return true; }
    return false;
}
...
if (!IsPublicKeyMatching(fstab_entry, public_key_data, preload_avb_key_blobs)) {
    if (!allow_verification_error) {
        LERROR << "Unknown public key is not allowed";
        return nullptr;                       // -> SetUpStandaloneAvbHashtree fails
    }
}
```

So on a locked bootloader a GSI boots only if its AVB key is one of the
`.avbpubkey` blobs in the **stock ramdisk `/avb`**. Nothing else matters:
not the image contents, not DSU's install-time checks, not the super partition.

Third gotcha, also fatal: a DSU image must carry a hashtree descriptor named
`system`; `avbtool` only writes it when `--partition_name system` is passed.

### 3. Open question that decides everything

Which keys are in the stock ramdisk `/avb`? OEMs that inherit
`$(SRC_TARGET_DIR)/product/developer_gsi_keys.mk` ship Google's Developer-GSI
public keys there, and then **official Google GSIs boot on a locked device with
no writes at all**. Otherwise only a Bigme-signed image (private key not
available) can boot while locked.

Read it without writing anything:

```bash
tools/inspect-avb-keys.sh          # live state
tools/inspect-avb-keys.sh --dump   # + BROM read of boot_a/b, lists /avb keys
```

Compare the sha1 of each key blob with
`python3 gsi/avbtool.py info_image --image <gsi>`.

## Route B (2026-11-18): put the GSI key in the boot ramdisk

`fs_mgr` on a locked device only checks that the DSU image's embedded vbmeta key
is one of the blobs under `avb_keys=/avb`; it does not care which key we use
for slot `_b`, as long as the bootloader accepts it. The stock images show:

| image | embedded key sha1 | note |
|---|---|---|
| `vbmeta_a`, `vbmeta_b` | `cdbb7717…617d` | AOSP AVB test key; private key ships in AOSP (`external/avb/test/data/testkey_rsa2048.pem`) |
| `vbmeta_b`, `boot` chain | `9d808b09…3d99` | Bigme boot key; changed by route B |
| `boot_a`, `boot_b` footer | `9d808b09…3d99` | |
| TrebleDroid GSI footer | `cdbb7717…617d` | same as top-level vbmeta → no GSI re-sign needed |

So the AOSP test key *is* the device root of trust, and `boot_b` can be
rebuilt with a modified ramdisk and signed with it.

Two AOSP details make the ramdisk patch work:

* `TransformFstabForDsu()` (`fs_mgr/libfstab/fstab.cpp`) overwrites
  `entry->avb_keys = "/avb"` for every DSU partition, and
  `AvbHandle::LoadAndVerifyVbmeta()` treats that as a *directory*: every file
  in it is compared (length + bytes) against the image's embedded key.
* `ro.boot.force_normal_boot=1`, so init chroots into `/first_stage_ramdisk`
  before first-stage mount; `/avb` therefore resolves to
  `/first_stage_ramdisk/avb`. The key is installed in **both** places.

The stock `first_stage_ramdisk/fstab.mt6765` already names
`/avb/q-gsi.avbpubkey:/avb/r-gsi.avbpubkey:/avb/s-gsi.avbpubkey` for `system`,
but the stock ramdisk contains **no `/avb` at all** — the GSI keys were never
packaged. That is why every non-Bigme DSU boot died before Android started.

Route B output (`tools/build-avb-anchor.sh`):

* `boot_b.new.img` — stock header/kernel byte-identical; ramdisk is the stock
  cpio plus a second concatenated cpio adding
  `avb/cdbb77177f731920bbe0a0f94f84d9038ae0617d.avbpubkey` and the same file
  under `first_stage_ramdisk/avb/`; footer re-signed with the AOSP test key,
  salt/rollback/flags/props copied from stock (asserted by the script).
* `vbmeta_b.new.img` — stock descriptors byte-identical except the `boot`
  chain key going `9d808b09…` → `cdbb77…`; re-signed with the same key. The
  hashes for `dtbo`, `product`, `system_ext`, `system`, `vendor` and the other
  chain keys are untouched (`tools/avb-compare.py` proves it).

Useful side effect: on every boot `CopyDsuAvbKeys()` copies `/avb/*` to
`/metadata/gsi/dsu/avb`, exactly the directory the DSU *installer* uses for its
key/revocation check — install and boot agree.

Rollback is a non-issue: `fs_mgr`'s `RollbackDetected()` is a no-op
(`// TODO(bowgotsai): Support rollback protection.`) in android-14, so the
GSI's `rollback_index 1749081600` at location 0 is neither compared nor stored.

Safety: only `boot_b` and `vbmeta_b` are written. Slot `_a`, `super`, the
`vbmeta_*` sub-partitions and `userdata` are untouched; stock dumps are
checksummed before and after, and `tools/flash-route-b.sh revert` writes the
byte-identical stock images back over BROM.

`gsi/system-testkey.img` (re-signed with `de54aa1a…`) is **obsolete**: the GSI
already carries `cdbb77…`, the key we can install into `/avb`.

| artifact | size | sha256 |
|---|---|---|
| `build/route-b/boot_b.new.img` | 33554432 | `53eaf910c5fc36e9b2f569d6221f208ee81783b7c1428f22adec859507f52f1a` |
| `build/route-b/vbmeta_b.new.img` | 8388608 | `e0beda83d35f9f0a6fcb96967cf2436441baf27337452029d9812e5265b4c198` |

## First GSI boot (2026-11-18)

Booting the DSU took ~184 s to adb. Running system:

| item | value |
|---|---|
| fingerprint | `google/treble_arm64_bvS/tdgsi_arm64_ab:16/BP2A.250605.031.A3/250617:userdebug/test-keys` |
| AVB | `green`, `flash.locked=1`, `veritymode=enforcing`, slot `_b` |
| display | 720x1440 @ 60 Hz, density 276, `hwcomposer.mt6765` |
| GPU | PowerVR Rogue GE8320, GLES 3.2 |
| vendor HALs | audio.primary.mt6765, camera.provider@2.6, gralloc.rogue, sensors, thermal, gnss, bluetooth, lights, power, vibrator |
| other | BT ON, 2 cameras, accelerometer/gyro/mag/light sensors, battery 87 %, `adb root` works |
| DSU state | `running / installed / disabled` → single-boot, next reboot returns to stock |

Known noise: `/vendor/bin/thermal_manager` SIGSEGVs once at boot (vendor binary
vs. Android 16 GSI); it is not respawned. No EPD-specific HAL exists under
`/vendor` — the e-ink panel is driven as an ordinary MTK DSI display through
`hwcomposer.mt6765`, so expect normal-Android refresh behaviour rather than
stock's EPD waveform handling.

`gsi_tool status` = running while the DSU boots means the one-shot flag worked.
To stay on the GSI across reboots, run `gsi_tool enable` (no `--single-boot`)
from the GSI (root) or from stock before rebooting.

## GSI display: e-ink panel showed only the boot logo (2026-11-18)

Symptom: the Android 16 GSI booted and `screencap` showed a normal launcher,
SurfaceFlinger reported the display `state ON`, but the physical panel kept
showing the bootloader logo, so the UI was invisible and the phone looked
unusable.

Diagnosis:

- The panel is driven by the stock vendor composer
  (`/vendor/lib64/hw/hwcomposer.mt6765.so`, process
  `composer@2.1-se`) through the kernel `eink` driver (TPS65185 PMIC,
  `/sys/kernel/debug/eink_debug`, kernel threads `[eink pixel proc]` and
  `[eink debug sys]`).
- On stock every frame produces:
  `I hwcomposer: [DEV] [normal commit]: mode=b1,update_area=(0,0,1439,719),...`
  On the GSI only `dirty ratio: 1.0` and `[POLICY] hwcCommitInfo: 0,refreshMode:
  0xb2` appeared — the e-ink engine was never handed a frame, so it kept the
  last image (the boot logo).
- The composer's e-ink commit/idle thread is gated on
  `persist.vendor.xrz.fps` / `fps_high` / `fps_low` (binary strings include
  `idle thread doesn't create due to 0 refresh rate`). Those props are set by
  Bigme's patched **stock framework** (`framework.jar` contains
  `xrz.screen_type`), which the GSI replaces; on the first GSI boot they were
  empty.

Fix (applied; `persist.*` so it lives in `/data/property`):

```sh
adb root
adb shell setprop persist.vendor.xrz.fps 120
adb shell setprop persist.vendor.xrz.fps_high 38
adb shell setprop persist.vendor.xrz.fps_low 30
adb shell setprop ctl.restart vendor.hwcomposer-2-1   # or just reboot
```

Result: opening Settings produces `[normal commit] mode=b2` and the panel
updates; touch (`fts_ts_b531`) is enabled and reports coordinates.

Still missing vs stock (cosmetic, composer falls back to defaults):

- `ro.vendor.xrz.*`: `screen_type=0`, `default_refresh_mode=178`,
  `phy_rotation=180`, contrast/brightness/text-enhance defaults. Setting them
  at runtime works (they were unset, so `setprop ro.*` succeeds) but they do
  not survive a reboot. Persisting them needs a prop file in the boot ramdisk
  or an edit to the DSU system image.
- Stock commits use `mode=b1`, the GSI uses `b2` (composer default).
- Stock's "E-ink Center" depends on the patched framework; there is **no
  vendor Binder service** for it (`service list` has nothing xrz/eink), and
  writing `vendor.xrz.global_refresh_mode` does not change the composer's
  selected mode. A replacement refresh-control UI would have to be built
  against the composer/kernel interface, not just props.

Stock's `/system/ro.prop` and `/system/rw.prop` were pulled to
`build/eink-props/` for reference; they contain only RSC paths, not the xrz
props.

## Android 17 GSI experiment (2026-11-18)

Decision: keep the working Android 16 GSI as the fallback, test an Android 17
GSI next, then decide where to invest in e-ink work.

Source: Google's official AOSP ARM64 GSI, Android 17 (cinnamonbun), build
`DP11.261030.003`:

```
https://dl.google.com/developers/android/cinnamonbun/images/gsi/
  aosp_arm64-exp-DP11.261030.003-16847878-1cb1c525.zip
sha256 1cb1c5254299581246ba626faccd41566df25382cd0415d8b82bf2e2d80cbf6c
```

Saved under `gsi/a17/`. It is an `-exp` (userdebug/test-keys) build, so its AVB
key should be the AOSP test key (`cdbb77…`, sha1 of
`/var/tmp/avb-testkey.avbpubkey`) that the Route B ramdisk already trusts —
verify with `avbtool info_image` before installing.

DSU supports only one installed image (`ro.gsid.dsu_slot=dsu`), so this
replaces the A16 DSU. The A16 image and tooling stay on the PC
(`gsi/system-td-arm64-vanilla.img`, reinstall in ~3 min) and the DSU userdata
is disposable (the xrz props live there, so they must be re-applied after a
fresh install).

Sequence:

1. `gsi_tool disable` from the A16 GSI, reboot to stock.
2. `gsi_tool wipe` on stock (frees ~10 GB).
3. `tools/dsu.sh install -i gsi/a17/system.img --skip-hash --single-boot`
4. Boot, check `ro.build.fingerprint`, then re-apply the e-ink props
   (`persist.vendor.xrz.fps*`) and confirm the panel commits.

## BROM workflow gotcha (2026-11-18)

`mtk.py w` closes the DA session **without resetting the SoC**. The phone then
sits in preloader/DA mode (`0e8d:2000`) and looks dead: no boot, no adb, and a
second `mtk.py` invocation fails its handshake because the resident DA does not
answer BROM. Only a physical 20 s power-button cycle clears it.

Fix used by `tools/flash-route-b.sh` and `tools/recover-stock.sh`: one session
with both commands, so the DA powers the phone off cleanly at the end.

```bash
mtk.py multi "w boot_b,vbmeta_b boot_b.new.img,vbmeta_b.new.img;reset"
```

`reset` sends SHUTDOWN with `bootmode=0` (power off). Press power once to boot.
`tools/recover-stock.sh` wraps the stock write in a retry loop for the case
where adb is gone and BROM only shows up in short windows.

Second trap: mtkclient writes a `.state` file in the working directory at the
end of DA setup. On the **next** fresh connection `DaHandler.connect()` sees it
and calls `daloader.reinit()` instead of uploading the DA, which then fails
with `Status error: Wrong magic`, USB I/O errors and an empty GPT
(`Couldn't detect partition`). Both scripts now `rm -f .state` before each
session.

## Next steps (route B)

1. ~~Flash `boot_b` + `vbmeta_b`~~ — done, boots green/locked/enforcing.
2. ~~Install the DSU with `--single-boot`~~ — done (`installed,enabled`), one-shot
   armed with `gsi_tool enable --single-boot`.
3. Booting the TrebleDroid Android 16 GSI now. If it comes up, check the
   fingerprint is `google/treble_arm64_bvS/…:16/…`, and then exercise what the
   MT6765 / vendor-11 stack actually supports: display + e-ink refresh, touch,
   Wi-Fi/Bluetooth, audio, camera, suspend.
4. If the DSU fails, the phone falls back to stock on the next power cycle
   (single-boot). Capture the USB pattern (0e8d:2000/201c) and treat it as a
   first-stage failure; pstore is SELinux-blocked on the `user` build, so the
   options are UART, an unlocked bootloader, or `dmesg` once/if the GSI starts.
5. `tools/flash-route-b.sh readback` still worth doing at some point.

Fallbacks unchanged: bootloader unlock (still gated on
`sys.oem_unlock_allowed`), or ask Bigme for firmware/unlock support.

## Files

- `dsu.ps1` — Windows DSU install/status/enable/disable/wipe helper
- `tools/dsu.sh` — Linux port of `dsu.ps1` (no `pwsh` on the Arch box)
- `tools/build-avb-anchor.sh` — builds `build/route-b/boot_b.new.img` + `vbmeta_b.new.img`
- `tools/flash-route-b.sh` — verify / flash / readback / revert over BROM
- `tools/avb-verify.py` — standalone AVB signature + hash verification
- `tools/avb-compare.py` — proves a re-signed vbmeta only changed one chain key
- `tools/avb-splice-chain.py` — re-keys one chain descriptor and re-signs
- `tools/repack-boot.py` — boot image repack preserving every header field
- `tools/resign-gsi.sh` — correct AVB re-sign for DSU (forces `--partition_name system`)
- `tools/inspect-avb-keys.sh` — live state + read-only BROM `boot_a/b` `/avb` key dump
- `.venv-mtk/` — mtkclient python deps (recreated 2026-11-18 for python 3.14)
- `gsi/system-td-arm64-vanilla.img.xz` — TrebleDroid Android 16 GSI (verified sha256)
- `gsi/system-td-arm64-vanilla.img` — decompressed, key `cdbb7717…617d`
- `gsi/system-testkey.img` — correctly re-signed copy, key `de54aa1a…7b1d` (sha256 above)
- `gsi/avbtool.py`, `gsi/testkey.pk8` — AVB re-signing tools

## BROM metadata read (2026-11-18 21:16)

Read the 32 MB `metadata` partition over BROM to clear the DSU re-entry state:

```
mtk.py r metadata /tmp/metadata.img   ->  READ_OK 33554432
```

Result: **the whole partition is zeros** (md5 `58f06dd588d8ffb3beb46ada6309436b`,
0 non-zero bytes, no ext4/f2fs superblock at 0x400). So there is no
`/metadata/gsi/dsu/*` state left to delete — nothing to write back. Either the
failed A17 boots or an earlier `gsi_tool wipe` already cleared it, and `fs_mgr`
will re-format `metadata` on the next normal boot.

Consequence: skip the "modify + reflash metadata" step and just power the phone
on; the next boot should come up in stock (slot `_a`), not the hung DSU.

## Root cause of the post-A17 boot loop (2026-11-18 21:35)

The 32 MB `metadata` partition is **unused** on this device. The stock
first-stage fstab (`build/.../first_stage_ramdisk/fstab.mt6765`, also visible in
`backup/brom/boot_b/rd/`) says:

```
/dev/block/by-name/md_udc /metadata ext4 ... wait,check,formattable,first_stage_mount
```

so DSU state lives on **`md_udc`** (0x169a000 bytes @ sector 242752), not on
`metadata`. That is why the earlier metadata dump was all zeros — correct read,
wrong partition.

`md_udc` (dumped over BROM to `backup/brom/md_udc.orig.img`) contained a
**persistent** DSU install, not a one-shot:

```
/gsi/dsu/active         = "dsu"
/gsi/dsu/install_status = "ok"
/gsi/dsu/booted         = "1"
/gsi/dsu/lp_names       = "userdata_gsi,system_gsi,"
/gsi/dsu/avb/cdbb7717….avbpubkey
/gsi/dsu/dsu/{install_dir=/data/gsi/dsu/,metadata_encryption_dir,lp_metadata,complete=OK}
```

No `one_shot_boot` file ⇒ first-stage init re-mounted the dead A17 image on
every boot ⇒ hang before USB ⇒ LK reset. `expdb` proves the boot chain itself
was healthy:

```
[avb] cmdline = ...androidboot.vbmeta.avb_version=1.2 androidboot.vbmeta.device_state=locked
[avb] boot/recovery vfy time = 600 ms
[avb] avb_ret = 0
```

That matches the observed 0e8d:2000 → 3 s → reset, five times, then stop.

Also learned: `init_boot_b` and `vendor_boot_b` are **entirely zero** on this
device (8 MB / 64 MB of 0x00); the real ramdisk only exists in `boot_b`. Route B
targeting `boot_b` was right.

Repair (`backup/brom/md_udc.clean.img`, sha256 `66b6db5b…`): journal replayed
with `e2fsck -fy`, then only `/gsi/dsu/*` removed with `debugfs -w`, keeping
`/vold/metadata_encryption/{key,dsu}` so userdata stays decryptable. Written
back over BROM in one session with `reset`. Result: stock booted,
`sys.boot_completed=1`, `verifiedbootstate=green`, `slot _b`, `gsi_tool status`
= normal, free space back to 13 GB.

Lesson: never `wipe`/format `md_udc` — it holds the userdata metadata-encryption
key blobs. Delete `gsi/dsu` only.

## A16 GSI back up + working E-ink Center (2026-11-18 22:50)

**The A17 hang was not DSU.** After wiping `md_udc`, stock booted; installing the
*vanilla* A16 image as a persistent DSU booted in ~3 min on the first try
(`google/treble_arm64_bvS/…:16/…`, slot `_b`, `gsi_tool` = running/installed/
enabled). The rebuilt `system-td-a16-eink.img` is what hung first-stage: the
kernel log in `expdb` stops right after `__mount(md_udc,/metadata)=0` and never
reaches `[libfs_avb]`, then the AP watchdog resets. AVB of that image verifies
locally, so the repack (debugfs writes + re-sign) is suspect. Do not trust the
rebuilt image; apply e-ink settings at runtime instead.

Useful: `expdb` holds kernel *and* first-stage init logs, not just LK. Dump it
over BROM (`mtk.py r expdb`) — it is the only post-LK log available on this
locked `user` build. Watch for `[pmic_check_rst] AP Watchdog` to tell a hang
from a clean reboot.

### E-ink fix that actually works

Props alone are not enough. Both are required:

```sh
setprop persist.vendor.xrz.fps 120
setprop persist.vendor.xrz.fps_high 38
setprop persist.vendor.xrz.fps_low 30
service call SurfaceFlinger 1008 i32 1     # mDebugDisableHWC, root only
```

Then `logcat -s hwcomposer` shows `[DEV] [normal commit]: mode=b1,...` and the
panel updates. Without the SurfaceFlinger call the composer only logs dirty
ratios and `[POLICY] skip`, and the panel keeps the boot logo.

`vendor.xrz.global_refresh_mode` does steer the composer (`177` -> `mode=b1`,
`178` -> `mode=b2`); `/sys/kernel/debug/eink_debug/global_mode` accepts writes
as root but does not by itself change the commit mode. `global_mode`,
`contrast`, `brightness`, `saturation`, `global_dither`, `anti_flicker` all
accept root writes even though they are mode `0444`.

### App

`build/eink-center/` — `eink-center.apk` (UI) + `su-stub.apk` (phh-su
responder). `BootReceiver` re-applies the panel fix at 8/15/30/60 s after boot,
so the panel works without touching anything. Verified: after a cold reboot the
GSI comes up with 57 `normal commit` lines and no manual commands.

phh's su only allows `shell`/`system`/`radio` by default and asks
`me.phh.superuser` (Koush protocol) for apps — an app that is not shipped in
phh's releases. The stub implements it: su passes a `socket` extra, the request
arrives as a flattened bundle (`from.uid`, `command`, `eof`), and the reply must
be exactly `ALLOW` or `DENY` (uppercase). `allow` gives `Permission denied`
everywhere; that wasted a cycle. Also note Android 11 package visibility: the
stub needs `<queries>` for `com.hibreak.einkcenter` before `getPackageUid()`
returns anything.

Boot-time caveat found in testing: a `BroadcastReceiver` that calls
`goAsync()` and then sleeps through a retry schedule trips the 60 s broadcast
timeout (`ANR in ... Reason: Broadcast of Intent ... BOOT_COMPLETED`). The
receiver now only schedules `AlarmManager.setAndAllowWhileIdle()` alarms and
each alarm does one quick fix. After that: boot -> 15 normal commits, 4 su
grants, 0 ANRs, nothing touched by hand.

E-ink Center 0.2: Quick Settings tile (`EinkTile`, tap = b1/b2), editable
composer fps (base/high/low, default 120/38/30), explicit "Switch to stock"
(`gsi_tool disable` + reboot, verified reversible with `gsi_tool enable`), and a
card layout.

Hardware limits confirmed on this unit:
* frontlight is a single PWM channel — `/sys/class/leds/lcd-backlight` has only
  `brightness`/`max_brightness`; no warm/CCT node anywhere in sysfs, the device
  tree or `lights.mt6765.so`. Dual brightness with warm light is not available.
* the "53/60 fps" idea was never implemented; the composer props are 120/38/30.
  The fps fields let you try other values, and `service call SurfaceFlinger 1008
  i32 1` is re-issued after applying them.

## Hyper / dual frontlight findings (2026-11-18 23:30)

From matanmi12/bigme-b1051c-android16 (HiBreak **Pro**, MT6877) kernel RE, and
confirmed against this unit:

* Modes: HD 256 = 2147483825 (0x800000B1), Fast 178, Extreme 179, Regal 180,
  Hyper 183, A2 16, GC4 8, DU 2, GL16 32, CLEAN 176.
* `vendor.xrz.force_global_refresh_mode` is **one-shot** (HWC resets it to -1
  after a frame); `vendor.xrz.global_refresh_mode` is the live global override.
  Our `setMode` writes force=0 then global, which is correct.
* Hyper gate in the kernel: `is_support_hyper_mode() = w*h < 2,123,521`. Our
  1440x720 (1,036,800) passes; the Pro's 2496x1944 (4,852,224) fails and Hyper
  freezes that panel. Do not copy "Hyper is broken" conclusions to this model.
* Measured commit rate (10 s benchmark activity): Fast 6-7/s, Extreme 10/s,
  Hyper 24-28/s, everything else ~6/s. Setting `persist.vendor.xrz.fps` to 60
  or 120, `cur_fps`, `fps_limit`, `disable_eink_vsync*` changed nothing.
  `debug.sf.enable_{gl,hwc}_backpressure=0` + phh equivalents: 24 -> 28/s.
* `/proc/device-tree/eink`: width 1440, height 720, `eink_max_fps=0x0f` (15),
  `eink_dynamic_fps=1`, `eink_fresh_hz=0x55` (85), `panel_type=0x3a`.
* The panel's frontlight is an **LM3630A** with separate cold/warm channels at
  `/sys/devices/platform/11009000.i2c2/i2c-2/2-0036/lm3630a_{cold,warm}_light`
  (0-255, 0660 root). The earlier "single channel only" conclusion was wrong —
  we had only looked at /sys/class/leds. Both channels verified writable.
* `clean_frequency` (full refresh every N page-sized updates) only counts while
  `vendor.xrz.check_commit != 0`; stock pulses it for 800 ms on user input.
  Not implemented yet, so the auto-clean counter stays idle.
* Bigme's 60 fps claim could not be checked: reddit returns 403 to this host.

## Stock switch, frontlight path bug, SystemUI (2026-11-19 00:05)

* **Switch to stock verified end to end**: from the GSI, `gsi_tool disable` ->
  status `disabled` -> reboot -> stock booted (`0e8d:201d HiBreak`). The app now
  re-reads `gsi_tool status` and only reboots on `disabled`, falling back to
  removing `/metadata/gsi/dsu/booted`. Note the DSU state survives: `active`,
  `booted` and `lp_names` stay, only `install_status` becomes `disabled`.
* **Frontlight writes were broken by shell globbing**: this sh does not expand
  a glob in a redirection target, so `echo N > /sys/.../lm3630a_cold_light`
  failed with "No such file or directory" while reads (arguments) worked. Fix:
  `P=$(ls ... | head -1); echo N > "$P"`. Verified cold=180/200 round-trip.
* **SystemUI plugin is not installable on this GSI**: the plugin needs
  `com.android.systemui.permission.PLUGIN`, whose protectionLevel is `0x2`
  (signature only). SystemUI is signed with `27:19:6E:38:6B:87:...:3D:FA`
  (`gsi/platform.x509.pem`), and no matching private key exists on this machine
  (`gsi/testkey.pk8` is the AOSP testkey pair, `61:ED:37:...`, which AOSP
  publishes but SystemUI does not use). A privileged app cannot be granted a
  pure signature permission, so the Pro's shade-slider plugin is out unless
  SystemUI itself is replaced.
* Instead, e-ink SystemUI aids that need no signature: `settings put global
  disable_window_blurs 1` (opaque shade; verified solid black),
  `accessibility_high_text_contrast_enabled 1`, and animation scales 0. All
  three are toggles in the app's new "SystemUI (e-ink)" card.

## ANR root cause + frontlight curve (2026-11-19 00:12)

ANR trace (input dispatch timeout on a touch at x=151,y=860) showed the main
thread blocked in `Root.run` -> `read()` called from
`MainActivity.lmWrite` from `SeekBar.onStopTrackingTouch`: the slider setters
executed `su` **synchronously on the UI thread** and returned its output as a
"command" for `run()`. Every slider release therefore stalled input dispatch
for the duration of a su round-trip (hundreds of ms to seconds), which is a
guaranteed ANR. Fixes:

* `sysfs()` / `lmWrite()` now only *build* command strings; execution stays on
  the worker executor.
* Slider moves use `runQuiet()` (no full re-read), halving su traffic.
* EinkDaemonService is started once per process (`daemonStarted`), not on every
  onResume; the ANR trace showed 10+ repeated SvcStart entries.
* Frontlight now uses the stock Bigme 37-step table (0,2,20,...,222) instead of
  a linear 0-255 slider, and displays `step/36 (raw code)`. Verified: cold 80,
  warm 40 from the UI; 10 rapid drags -> 0 ANRs.

Also: `cold`/`warm` were missing from the batched reload entirely, so the
sliders always drew 0 even after the hardware changed - that was most of the
"hard to control" feeling.

Second ANR (00:13) had the same stack shape but a different slider: the
frontlight **Level** setter was still `v -> su("echo ... > lcd-backlight")`,
i.e. it executed su on the UI thread while every other setter only built a
string. Fixed, plus a defensive change: `slider()` and `frontlight()` now call
`setter.apply(v)` *inside* the worker thread, so a setter that touches root can
never block input dispatch again. Audit: `grep 'v -> su('` is empty.
Stress test: 18 drags over Level/Cold/Warm/Contrast/Brightness/Saturation ->
0 ANRs, channels end at 243 / 218 / 218.

## QS tiles (2026-11-19 00:20)

Three Quick Settings tiles replace the stock E-ink Center shortcuts, since the
SystemUI plugin route is blocked by the platform key:

* `EinkTile` ("E-ink fast/hyper"): tap toggles Fast 178 <-> Hyper 183.
* `CleanTile` ("E-ink refresh"): full refresh (screen off/on + HWC)
* `LightTile` ("Frontlight"): toggles the LM3630A cold/warm channels between
  saved levels (default 80/40) and 0.

`settings put secure qs_tiles "internet,bt,flashlight,modes,alarm,airplane,
controls,custom(com.hibreak.einkcenter/.EinkTile),custom(com.hibreak.einkcenter/.CleanTile),custom(com.hibreak.einkcenter/.LightTile)"`
pre-loads them; verified the first tile renders in the shade ("E-ink hyper").
The remaining pages keep SystemUI's defaults, so the user can rearrange with
the pencil. Tiles are the ceiling without touching SystemUI: a real brightness
slider replacement needs the PLUGIN signature.

## Stock refresh-rate baseline (2026-11-19 07:39)

Measured on **stock Android 14** while at stock, with the E-ink Center
`BenchmarkActivity` (continuous full-width animation, `FLAG_KEEP_SCREEN_ON`)
for 10 s per mode, counting `[DEV] [normal commit]` in `logcat -s hwcomposer`.
The `[HWCDisplay] fps:` line always reads ~60 Hz (vsync); the e-ink update rate
is the commit count.

| `vendor.xrz.global_refresh_mode` | stock name | composer `mode=` | commits/s | `vendor.xrz.cur_fps` |
|---|---|---|---|---|
| 180 | Regal | `b1` | 8.6 | 8 |
| 2147483825 | HD / 256 | `b1` | 8.5 | 7 |
| 178 | Fast | `b2` | 9.4 | 8 |
| 179 | Extreme | `b3` | 31.0 | 20 |
| 183 | Hyper | `b7` / `11` | **66.0** | 35 |

The same benchmark under the TD Android 16 GSI gave Hyper 24-28/s and
Extreme 10/s, i.e. the stock vendor path is roughly 2-6x faster at the top
modes. This makes the vendor-aware composition/refresh work in the LineageOS
HBPC GSI (forced client composition, `ro.bigme.force_client_composition`,
profile transactions) worth pursuing.

## LineageOS 23.2 HBPC GSI experiment (2026-11-19 07:46)

Source: `dianqk/LineageOS_gsi_HBPC`, release `v23.2-20261031`, vanilla EXT4
image (`LineageOS-23.2-20261031-HBPC-VANILLA-EXT4-GSI.img.xz`, 810,503,704 B,
sha256 `be807fd71959e9feb02e4bcd282215ed4ee836fd87eda700f97d7b8cb5143d45`,
raw 2,497,826,816 B). It targets the HiBreak **Pro Color** (MT6877, 824x1648
Kaleido 3, Android 12 vendor), not this MT6765 HiBreak, but it is a system-only
GSI and its e-ink stack is the same XRZ family.

Useful findings:

* The released image is **already signed with the AOSP AVB test key**
  `cdbb77…` (`avbtool info_image`: hashtree `Partition Name: system`,
  key sha1 `cdbb77…`, flags 0). Route B's `/avb` anchor therefore accepts it
  without re-keying; a re-sign with `/var/tmp/avb-testkey_rsa2048.pem` is only
  needed after modifying the image.
* The E-ink daemon (`/system/bin/a9_eink_server`) and
  `/system/etc/init/a9_eink_daemon.rc` hard-code the Pro Color frontlight at
  `/sys/devices/platform/11d01000.i2c7/i2c-7/7-0036/...`; this HiBreak uses
  `/sys/devices/platform/11009000.i2c2/i2c-2/2-0036/...` (same string length,
  binary + rc patched in place).
* Pro Color props in `/system/build.prop`: `display_policy_version=12`,
  `phy_rotation=270`, `screen_type=61`, `support_eink_animation=1`,
  `fps_low=40`, `fps_high=50`. Stock HiBreak values are 10 / 180 / 0 / 0 /
  30 / 38. The test image (`LineageOS-23.2-20261031-HBPC-HIBREAK.img`) was
  patched to the HiBreak values plus the stock `ro.vendor.xrz.default_*`
  defaults, then re-signed (no FEC) with the same key:
  size 2,477,694,976 B, hashtree `system`, key `cdbb77…`, no FEC.
* DSU install (`--single-boot`, 8 GB userdata) succeeded and the image was
  enabled, but the phone did **not** reach adb: it sits in MediaTek preloader
  mode `0e8d:201c`. One-shot boot should fall back to stock on the next power
  cycle. Pending: power-cycle, then dump `expdb` over BROM for the
  first-stage/init failure before deciding whether to retry the image
  (unmodified vs. patched) or revert to the TD A16 GSI.
* The HBPC GSI's `SLEEP-SCREEN.md` is the real fix for the blank-on-lock
  symptom: a white sleep page is drawn before display-off, SystemUI waits for
  the frame (present fence + 400 ms settle) and only then requests
  `power mode 0`; `config_displayColorFadeDisabled` removes the black cover
  frame. Android AOD is deliberately not used.

## HBPC GSI verdict + TD A16 restored (2026-11-19 10:15)

**HBPC image:** the untouched release boots on this HiBreak, but it is
securized (`ro.debuggable=0`, `ro.adb.secure=1`, `persist.sys.usb.config=none`)
and its UI is wrong for our 720x1440 panel (oversized/cropped). A
re-signed-only image (erase footer + `add_hashtree_footer`, no FEC, no content
edit) also boots, proving no-FEC is fine. Every attempt that changed early
`ro.*` values in a boot ramdisk or the system image failed to boot:
`screen_type=0`/`phy_rotation=180` (panel props) and even adb-only props
(`ro.debuggable=1`, `ro.adb.secure=0`) stop the HBPC image. The HBPC framework
is tied to its own display/composition path. **Do not treat it as a portable
system for this device.**

The TD A16 GSI was restored as a persistent DSU (`gsi_tool enable`), display
composer back in 120 Hz mode. The current `boot_b` is `boot_b.adb.img` (Route-B
keys + adb props + TCP 5555), not the clean `boot_b.new.img`; revert with
`tools/recover-stock.sh`/BROM if a pure boot chain is wanted.

**60 fps:** stock Hyper measured 66 commits/s. The TD GSI is capped at
24.4 commits/s; `debug.sf.enable_{gl,hwc}_backpressure=0`, phh backpressure
false, `vendor.xrz.disable_eink_vsync=1`, `min_refresh_rate=120`,
`fps_limit=0`, HWC on/off and `ro.bigme.force_client_composition=true` only
reach 27-28 commits/s. The stock/HBPC 66/s requires Bigme's patched
SurfaceFlinger/composer (source-level), not a prop.

**Blank-on-lock fixed:** enable the phh AOD overlay
`me.phh.treble.overlay.misc.aod_systemui`, `doze_always_on=1`,
`doze_enabled=1`, `debug.doze.aod=1`; the lock state becomes
`DOZE_SUSPEND` and the panel shows the black clock page instead of blanking.
`debug.doze.aod` is not persistent, so `BootReceiver` now re-applies the
whole set on every boot (plus `stay_on_while_plugged_in=0`).

**Portable HBPC pieces (tested):**

* `/system/priv-app/A9AccessibilityService/A9AccessibilityService.apk`
  (`net.dianqk.hibreak.eink`) declares `sharedUserId=android.uid.system` and
  is LineageOS-platform signed. The manifest's shared UID is a UTF-16 string
  in the binary XML; replacing it with `android.uid.systeX` (same length),
  keeping `resources.arsc` uncompressed, zipaligning and re-signing with the
  E-ink Center debug key makes it install as a normal app. `SettingsActivity`
  runs on TD (screenshot: E-Ink settings / frontlight / app display).
* Native `/system/bin/a9_eink_server` uses abstract socket `a9_eink_socket`
  and rejects clients whose uid is not 1000. The check is
  `cmp w3,#0x3e8; b.eq accept` at `0x4280`; replace the `b.eq` at `0x4284`
  with unconditional `b 0x42ac` (`0a000014`), patch the two lm3630a paths to
  `11009000.i2c2/i2c-2/2-0036`, and run as root. The app then reports
  `HiBreak E-Ink control service ready` and applies profiles; frontlight
  sysfs still reads 0/0 (the HBPC app's saved 37/19 does not reach our
  LM3630A), but E-ink Center's frontlight works.
* The app's Sleep screen / picture / static keyguard / outlined Settings /
  launcher / dialer / 60 fps are **SystemUI/framework/source** features and
  do not work on TD. AOD is our equivalent for the lock screen.
* The patched app and daemon live in `/data`; `BootReceiver` starts the
  daemon with `killall a9_eink_server; nohup /data/local/tmp/a9_eink_server`.
  Do not use `pkill -f /data/local/tmp/a9_eink_server` from the same shell:
  it matches the shell command line and kills itself.

## Source-build setup paused (2026-11-19 11:00)

The prebuilt HBPC image cannot be adapted at runtime: the untouched image
boots, but any boot-time `wm density`/`wm size` or `ro.*` panel/adb change
makes it reset to preloader after ~30 s. The only path to the HBPC UI +
60 fps is a source build, which needs ~150-250 GB and hours; it was paused
because the user had to leave.

Prepared and waiting:

* `/root/hbpc-src` — HBPC patch/build repo; HiBreak adaptation committed
  (panel props `screen_type=0`, `phy_rotation=180`, `ro.sf.lcd_density=276`,
  LM3630A paths, verifier updated). Commit `ea80752`.
* `/root/lineage-23.2` — partial LineageOS 23.2 checkout, initialized at the
  snapshot manifest commit `9776e518de506f610218c4caa6c1fddff46873af`; resume
  with `repo sync -c --optimized-fetch --no-clone-bundle --no-tags -j8`.
* `/root/eink_key` — extracted from the HBPC image, SHA-256 `69804441…`.
* `/root/keys` — AOSP testkey pair copied to releasekey/platform/shared/
  media/networkstack/bluetooth/nfc/sdk_sandbox (0600).
* `/root/android-sdk` — platform 35 + build-tools 35 shim + licenses for the
  TrebleApp Gradle step.

Build command (after sync):
`python3 /root/hbpc-src/hbpc/build-hibreak.py build --tree /root/lineage-23.2
 --variant vanilla --out-dir /mnt/backup/hbpc-out --keys /root/keys
 --eink-key /root/eink_key --treble-java-home /usr/lib/jvm/java-17-openjdk
 --android-sdk-root /root/android-sdk --gradle-user-home /root/.gradle
 --download-cache /root/hbpc-downloads --ccache-dir /mnt/backup/hbpc-ccache
 --dist /mnt/backup/hbpc-dist -j8`

Then re-sign `out/target/product/generic_arm64/system.img` with
`/var/tmp/avb-testkey_rsa2048.pem` (no FEC) and install as a DSU.

Phone state: the HBPC density test reset to preloader; a power cycle should
boot stock (the DSU was single-boot). Stock data untouched. The TD A16 DSU
that was working has been wiped by the experiments; restoring it is a ~5 min
DSU install once stock adb is reachable. AOD lock-screen fix and HBPC E-ink
app port were verified before the source-build detour.

## B1051C project vs HiBreak 60 fps (2026-11-19 11:20)

`matanmi12/bigme-b1051c-android16` is the closest public project: a
TrebleDroid A16 GSI on the stock vendor/kernel plus a Magisk module with an
HWC2 interposer (`hwcomposer.mtk_common.so`), `einkd`, framework RROs and an
E-Ink Control app. It is **not** a 60 fps shortcut for this phone:

* The shim's real HWC is hard-coded to `hwcomposer.mt6877.so`; the HiBreak
  vendor has `hwcomposer.mt6765.so`, so the prebuilt shim cannot load. The
  source (`shim/hwcshim.cpp`) can be rebuilt, but...
* The shim still requires SurfaceFlinger 1008 (HW overlays off) and commits
  the GPU client target, exactly like the TD workaround. It fixes panel
  commits/colour, not the 28 fps composer ceiling.
* Its E-Ink Control/SystemUI plugin is test-keys signed; the TD GSI's
  SystemUI is the custom platform key `27:19`, so the signature-protected
  SystemUI plugin cannot install (the app UI can).
* The B1051C is MT6877 / 300 dpi / Kaleido; its props (`screen_type=33`,
  `lcd_density=350`, policy 6, rotation 270) are not ours.

`right9code/hibigzero` is for the MT6765 HiBreak on **stock Android 14**
(power/refresh manager), not a GSI and not the 60 fps composer path.
`vbh.ai`/Evolution X HiBreak Pro GSIs are MT6877. HBPC is Pro Color source.

Conclusion: the 66/s Hyper path exists only in Bigme's stock A14 framework
and in the HBPC source build; no public prebuilt for the HiBreak MT6765.
The TD A16 GSI is ~28/s. Repo/module saved in `/root/b1051c` and
`/tmp/opencode/b1051c-module.zip` for the UI/RRO/shim reference.

## Last HBPC prebuilt option: PC-controlled via scrcpy (2026-11-19 11:31)

The only remaining use for the prebuilt HBPC image is **not as a phone**. The
untouched image boots and stays, but its panel scale is wrong; any density,
size or `ro.*` change resets it. A PC-controlled session would avoid the
phone panel: enable adb, run `scrcpy`, and drive the full 720x1440 framebuffer
on the PC (HBPC UI, sleep screen, 60 fps composer path).

A **minimal adb-only** image was built to test that:

* `gsi/lineage-hbpc/LineageOS-23.2-20261031-HBPC-ADBONLY.img`
  (2,477,694,976 B; signed `cdbb77`, no FEC).
* Added only `/system/bin/hibreak-adbonly.sh` + init rc: writes
  `/data/misc/adb/adb_keys` with the host key, sets `adb_enabled=1`,
  `persist.sys.usb.config=adb`, `service.adb.tcp.port=5555`. **No** density,
  size, display or `ro.*` changes.
* Untested. Install single-boot (`tools/dsu.sh install -i ... --single-boot
  --reboot`). If Android stays up and adb appears, `scrcpy` can use the HBPC
  UI on the PC; if it resets to preloader, the prebuilt HBPC is unusable and
  only the source build remains.
* Even if it works, the phone panel is still Pro-Color-scaled; it is a
  PC/desk device, not a pocket phone. The source build is the only fix for
  the panel.

## HBPC SurfaceFlinger swap on TD A16: result (2026-11-19 12:25)

**Failed for a fundamental reason, but we proved exactly where the line is.**

What worked:
* The HBPC `surfaceflinger` (with `EinkRender`/`EinkClear`/`hyper dither`)
  loads and runs on TD after:
  - staging it in `/system/xbin` (tmpfs; `/data` is `nosuid`, so init's
    domain transition was denied with `nosuid_transition`),
  - bind-mounting six HBPC libs for the load only
    (`libgui`, `libui`, `libprotobuf-cpp-lite`, `libtimestats_atoms_proto`,
    `libSurfaceFlingerProp`, `libstatslog_surfaceflinger`),
  - providing HBPC's V7 graphics-common lib over TD's V6 path
    (patched NEEDED names; same length),
  - stopping zygote during the window (TD `libandroid_runtime` calls
    `SurfaceComposerClient::Transaction::setEarlyWakeupStart`, absent from
    HBPC `libgui`), then unmounting the libs and starting zygote.
* HBPC SF ran (`/proc/<pid>/exe` md5 matched) and produced a display.

Why it cannot work:
* `system_server` crash-loops ~11 s after boot:
  `Timeout waiting for default display to be initialized`, and SF logs
  `state.readFromParcel(&data)` / `getSupportedContentTypes ... UNSUPPORTED`.
* TD is AOSP 16 `BP2A.250605` (June 2025, has `setEarlyWakeupStart`);
  the HBPC SF is built from an **older AOSP/Lineage 23.2 SF AIDL** (its
  `libgui` lacks that symbol). The SurfaceFlinger/display binder interface
  differs in both directions, so client and server parcels cannot be mixed.
  No lib-shim can bridge a stable-AIDL version gap in both directions.

Consequences:
* TD A16 stays at ~23-28 fps with the stock SF. The HBPC path (66/s) needs
  SurfaceFlinger built from **TD's matching AOSP tag** with Bigme's e-ink
  patches, or the whole HBPC/Lineage system built for this panel.
* Keep `build/hbpc-sf-swap/` as the proven test harness (staging path,
  SELinux workaround, transient mounts, zygote stop) for a future SF build.
* Phone restored to TD by reboot; stock untouched.

Baseline on this boot: ~22.4-24.0 fps (FPS ring buffer, BenchmarkActivity).

## E-ink tuning on TD A16 (2026-11-19 13:05)

Measured with vendor logging (`vendor.xrz.check_commit=1`) and the
BenchmarkActivity / Settings scrolling:

* TD commits the **whole 1440x720 panel** every frame:
  `[normal commit]: mode=..,update_area=(0,0,1439,719)`.
* Panel IRQ: **42–47 ms** per update; commits ~42 ms apart → ~24 fps.
* Every refresh mode runs at the same rate (177/178/179/180/183/16 and
  0x800000B1): only image quality/ghosting changes, not throughput.
  Vendor modes seen: 178→b2, 179→b3, 183→0x11, 177/180→b1, 16→0x10.
* `debug.sf.latch_unsignaled`, `debug.sf.hwc.min.duration`,
  `disable_image_copy`, `fps_limit=120`, `disable_eink_vsync`, permissive
  SELinux: **no fps change** (all ~24).
* A small-damage test activity (invalidate only old/new 320x240 rects) still
  produced full-panel commits. With HWC composition the vendor errors
  `wrong layer number,num=3`; partial updates are not reachable on this GSI.
  `dirty ratio` was 0.85–1.0 in both cases.
* SELinux denials seen earlier were self-inflicted: the vendor only tries to
  clear `vendor.xrz.update_waveform`/`force_global_refresh_mode` after *we*
  set them. On a clean boot there are no denials and no per-commit waveform
  reloads. Do not leave those two props set.

Conclusion: ~24 fps is the full-panel e-ink update cost on this controller.
Stock's 60+ needs the patched SF damage/profile path, which requires a
matching source build. No runtime knob or app change can beat the full-frame
refresh time.

What was applied for usability (persisted in E-ink Center BootReceiver):
* animations 0, rotation locked, heads-up off, window blur off.
* `vendor.xrz.auto_clean_enabled=1`, `clean_frequency=10` (full refresh every
  ~10 dirty page turns; EinkDaemonService pulses `check_commit` on touch).
* Mode guide: 183 Hyper = fastest/no gray (scrolling); 178 Fast = gray text;
  177/0x800000B1 HD = 256-level dither; 180 Regal / 179 Extreme = quality.
* E-ink Center already exposes mode buttons, Clean/Full-refresh, frontlight
  (LM3630A cold/warm), fps props and auto-clean controls; EinkTile toggles
  178/183.
* For best perceived speed: paginated readers instead of scrolling, static
  wallpaper, minimal launcher, no rotation, and a full refresh after large
  screen changes.

## E-ink UI tuning on TD A16 (2026-11-19 13:20)

Visual changes applied (persist in /data; BootReceiver re-applies the boot ones):
* **Paper-white wallpaper** for home + lock (`WallpaperActivity` in E-ink
  Center, SET_WALLPAPER). This flips Launcher3/status bar/notifications to a
  light UI and makes Monet generate neutral colors instead of pastel tints.
* **High-contrast text** (`high_text_contrast_enabled=1`) and
  **font_scale=1.1** — Settings now reads as bold black on white with big rows.
* **Animations 0**, rotation locked, heads-up off, window blur off.
* **3-button navigation** (discrete taps, no gesture trail).
* **Quick Settings reduced to 8 useful tiles**:
  internet, bt, flashlight, rotation, battery, E-ink mode, E-ink refresh,
  Frontlight (via `cmd statusbar set-tiles`; `settings put` alone is
  overwritten by SystemUI).
* **Status bar cleanup** via `icon_blacklist`
  (rotate,volume,alarm_clock,zen,cast,hotspot,managed_profile,usb).
* **New apps no longer add icons to Home** (Launcher3 Home settings).
* Colors: disabled/kept the `com.android.systemui:neutral/accent/dynamic`
  theme-overlay group while testing; the QS tile panel still renders dark
  (SystemUI resource/theme, not wallpaper/night mode). Notification shade and
  launcher are light. Minimizing QS tiles is the practical mitigation.

Icon/animation notes:
* Launcher3 has `ThemedIconDrawable` and a `themed_icons` key, but setting it
  (`settings put secure themed_icons 1`) did **not** theme icons on this GSI;
  the launcher still shows full-color app icons.
* The home Quick Search Bar cannot be disabled from Home settings (only
  Notification dots, Add icons to Home, rotation are exposed).
* Best e-ink icon/UI answer is a **text-first launcher** (no icons, no search
  bar, no animations) or an icon-pack-capable launcher with a monochrome pack
  (e.g. Arcticons). A custom e-ink launcher can apply a saturation/threshold
  ColorFilter to icons and use paginated text lists.
* For animations, global scale 0 is the only system-wide control; app-internal
  animations and list fling physics remain. Prefer paginated readers and apps
  without transitions.

## Grayscale / anti-aliasing on the HiBreak panel (2026-11-19 13:25)

Panel: 5.84" 720x1440 mono E Ink, **16 native gray levels** (4-bit).
"256-level" is not per-pixel; it is spatial/temporal dithering.

Pipeline on TD: apps render 8-bit AA (Skia) -> SF client target RGBA (1008)
-> vendor `convertToGray` -> mode/dither -> panel waveform.

Driver findings:
* `/sys/kernel/debug/eink_debug/anti_alias` reports `support=1, value=0`
  (panel AA **off**). Root can write `1` to it; we set `1 1`. It resets on
  reboot, so E-ink Center BootReceiver re-applies it.
* Composer commit log (`dmesg | grep "update image"`):
  - Fast 178: `mode=0xb2(map=0x2), dither=0(def=3)` -> plain 16-level.
  - HD 0x800000B1: `mode=0xb1(map=0x84), dither=4(def=4)` -> dithered,
    best grayscale/AA appearance.
  - Hyper 183 = 0x11 and A2 = 0x10 are 1-bit; AA is thresholded away.
* The vendor repeatedly tries to set `vendor.xrz.gray_256_dither` and is
  denied by SELinux (`hal_graphics_composer_default` -> `vendor_default_prop`).
  `setenforce 0` is not supported by this kernel. We set the prop from root
  (`gray_256_dither=1`) and persist it; HD mode still selects dither=4 from
  the mode bits regardless.
* `persist.vendor.xrz.gpu_dither` is 5 (stock default); leave it.

Practical guidance:
* Text/reading: Fast 178 (+ AA) — smooth AA, no dither noise.
* Images/PDF/photo: HD 0x800000B1 — dither=4, best tonal range.
* Scrolling/animation: Hyper 183 or A2 16 — fastest but 1-bit, no AA.
* AA is generated by apps; it survives only in gray modes. The driver
  anti_alias flag adds panel-side smoothing but can add ghosting.

Enable at boot (BootReceiver): `echo 1 > .../anti_alias`,
`anti_alias_enabled=1`, `gray_256_dither=1`.

## Booklauncher + EinkBro on TD A16 (2026-11-19 13:35)

Installed from upstream releases:
* **Booklauncher 3.1.0** (`com.woody.booklauncher`, `BookLauncher-3.1.0-release.apk`)
  set as the default HOME (`cmd package set-home-activity
  com.woody.booklauncher/com.woody.booklauncher.HomeActivity`). Onboarding
  skipped; granted MANAGE_EXTERNAL_STORAGE, POST_NOTIFICATIONS, and enabled its
  `ReaderWatchService` accessibility service + `MediaWatchService` notification
  listener via settings/`cmd notification allow_listener`. Home renders
  black-on-white, big clock, shelves, All apps/Stats/Widgets/Mini apps.
* **EinkBro 16.7.1** (`info.plateaukao.einkbro`, arm64) installed and set as
  the default BROWSER role (`cmd role add-role-holder ... android.app.role.BROWSER`).

Critical fix: the vanilla TD image reported **"No WebView installed"**, so
EinkBro crashed on launch. The AOSP provider was present but not selected:

```
cmd webviewupdate set-webview-implementation com.android.webview
# Current WebView: com.android.webview 133.0.6943.137
# settings get global webview_provider -> com.android.webview
```

Verified EinkBro by serving a local test page over `adb reverse tcp:8080`:
serif text with AA, smooth gradient, 16-step ramp and small text all render
correctly. The device currently has **no network**: Wi-Fi has no saved networks
and the SIM slot is ABSENT, so real browsing needs Wi-Fi credentials
(`cmd wifi connect-network SSID wpa2 PASS`, or connect on-device).

Revert Home: `cmd package set-home-activity
com.android.launcher3/.uioverrides.QuickstepLauncher`.

## Frontlight master brightness fix (2026-11-19 13:42)

The E-ink Center "Level" slider wrote `/sys/class/leds/lcd-backlight/brightness`
(the MTK display-backlight class), which is not the HiBreak frontlight, so it
did nothing. The frontlight is the LM3630A at
`/sys/devices/platform/11009000.i2c2/i2c-2/2-0036/lm3630a_{cold,warm}_light`
(mode 0660 root).

Changes:
* E-ink Center "Level (both)" uses the stock 37-step LM3630A curve and writes
  the **same code to cold and warm**, then writes `Settings.System.SCREEN_BRIGHTNESS`
  so the app and Quick Settings stay in sync. Cold/Warm sliders remain as trims.
* EinkDaemonService registers a ContentObserver on
  `Settings.System.SCREEN_BRIGHTNESS`; the Quick Settings / Settings brightness
  slider now writes both LM3630A channels through `su` (debounced 120 ms).
  The saved brightness is applied once at service start.
* Verified: brightness 40 -> cold=60 warm=60; 100 -> 126/126; 120 -> 150/150
  (the 37-step table maps 0..255 to raw codes 0..222).

Note: `RootShell`'s persistent `su -c sh` did not apply the write even though
the su stub allowed it; `Root.run` (`su -c <cmd>`) works and is used for the
brightness path.

## Readers + lock-screen wallpaper cycler (2026-11-19 13:55)

Installed:
* **Readest 0.12.20** (`com.bilingify.readest`, arm64) - verified: light UI,
  "Start your library / Import Books". Books come in through the system file
  picker (SAF), no broad storage grant needed.
* **KOReader v2026.07.1** (`org.koreader.launcher`, arm64) - verified: opens
  the Quickstart guide; MANAGE_EXTERNAL_STORAGE and read/write storage granted.

Lock-screen screensaver:
* `EinkDaemonService` registers `ACTION_SCREEN_OFF`; on each lock it calls
  `LockWallpapers.next()`, which reads images from
  `/sdcard/Pictures/EinkWallpapers` (JPG/JPEG/PNG/WebP/BMP), center-crops to
  720x1440, applies grayscale + mild contrast, and sets
  `WallpaperManager.FLAG_LOCK`.
* Index is persisted (`eink` prefs `wall_index`); files are sorted by name and
  wrap around. Verified: index advanced 0->1->2->0 on successive locks, and
  the lock screen showed the circle/gradient test images.
* E-ink Center has a **Lock wallpapers** card: image count, "Next wallpaper",
  "Refresh". Folder contains three sample images (w1/w2/w3) and README.txt.
* E-ink Center manifest now declares READ_MEDIA_IMAGES and
  MANAGE_EXTERNAL_STORAGE; appop granted in the current TD data. If the DSU
  data is wiped, re-run: `appops set com.hibreak.einkcenter
  MANAGE_EXTERNAL_STORAGE allow` and `pm grant com.hibreak.einkcenter
  android.permission.READ_MEDIA_IMAGES`.
* Note: this changes the keyguard wallpaper, which is what is shown when the
  screen is woken. The doze/AOD layer still uses the black clock page from the
  earlier AOD fix; a wallpaper-on-AOD sleep image would need a separate AOD
  overlay.

## Frontlight off on lock (2026-11-19 14:00)

The LM3630A stayed lit while the panel was off because only the wallpaper
cycler reacted to `ACTION_SCREEN_OFF`. EinkDaemonService now:
* on SCREEN_OFF: writes 0 to both cold/warm, then advances the lock wallpaper;
* on SCREEN_ON: restores the Android brightness if the LightTile's
  `light_on` pref is true;
* `applySystemBrightness()` now checks `PowerManager.isInteractive()` live,
  so a missed screen-on broadcast cannot leave a stale "screen on" state
  (which previously caused brightness writes while locked / no write on wake).

Verified: awake 144/144 -> locked 0/0 -> woken 144/144; wall_index still
advances on each lock.

## Custom TD E-ink image (2026-11-19 14:15)

Built `gsi/system-td-eink.img` (3.0 GB raw, AVB SHA256_RSA2048, key cdbb77,
salt/rollback preserved; FEC omitted because the host `fec` tool is missing;
`avbtool verify_image` passes). Original `system-td-arm64-vanilla.img` is
untouched.

Contents:
* `/system/etc/eink/payload/` - EinkCenter, SuStub, BookLauncher, Jelly,
  Readest, KOReader, Aperture, Etar, Glimpse, Twelve, Recorder, Camelot,
  Backgrounds (129 MB).
* `/system/etc/eink/wallpapers/` - three samples + README.
* `/system/etc/eink/setup.sh` - first-boot install + settings/roles/AOD/
  frontlight/QS/WebView/accessibility/wallpaper seed, marker
  `/data/system/eink_provisioned_v1`. Dry-run on the live system installed all
  APKs and applied settings successfully.
* `/system/etc/init/eink.rc` - service started at `sys.boot_completed=1` with
  `seclabel u:r:phhsu_daemon:s0` (same domain as phh-ota.rc/exec).
* ext4 grown 2.18 -> 3.0 GB, shared_blocks unshared, files labelled
  `u:object_r:system_file:s0`, modes/uid/gid set.

Build: `build/eink-image/make-image.sh` (note: avbtool verify needs the image
named `system.img`; the script uses a symlink in /tmp/opencode/avbcheck).
Install: `tools/dsu.sh wipe` (from stock) then
`tools/dsu.sh install -i gsi/system-td-eink.img --skip-hash --reboot`.
A wipe loses DSU userdata; `/sdcard` backed up to `sdcard-backup/` (6.4 MB).

## Restored working TD A16 (2026-11-19 15:55)

The custom self-provisioning image was a dead end for this locked DSU:
* The bootloader requires AVB **FEC**; the original image boots, a no-FEC re-sign
  falls to fastboot.
* Added FEC via a locally built AOSP `fec` (external/fec RS + minimal image code;
  output matches the original FEC byte-for-byte), but the DSU installer then
  refused to start/complete on stock (VerificationActivity state/activity
  resolution errors) even with the screen awake. Multiple stock reboots and
  DSU resets did not restore a reliable install path.
* The custom image `gsi/system-td-eink.img` is kept for reference only.

Restored `gsi/system-td-arm64-vanilla.img` as DSU, then reran the validated
payload setup (`/data/local/tmp/eink-test.sh`): BookLauncher default home,
Jelly default browser, Readest, KOReader, Lineage suite (Aperture, Etar,
Glimpse, Twelve, Recorder, Camelot, Backgrounds), E-ink Center, wallpapers,
AOD/frontlight/QS settings. `/sdcard` restored from `sdcard-backup/`
(app data tgz also retained). Frontlight 0 while locked, restores on wake.

Pragmatic persistence path for future flashes: flash the original TD image,
then re-run the payload/setup bundle; it is validated and takes ~1 minute.

## HANDOFF — 2026-11-20

Fresh Linux session: read `AGENTS.md` and `HANDOFF.md` first. They contain the
current phone state, the HWC-shim objective, the dead ends (HBPC SF swap,
custom baked image, kernel/waveform), and the exact next commands. The B1051C
reference repo is copied to `refs/b1051c/`; the shim source/plan is in
`build/hwc-shim/`; `build/eink-provision/provision.sh` restores apps/settings
after a fresh TD flash.

## MT6765 HWC reverse engineering + new shim (2026-11-20)

The B1051C repo (`refs/b1051c`) is the right *architecture* but the wrong
*device*. Reading our own vendor library settles it.

Library: `build/eink/hwcomposer.mt6765.so` (also `build/hwc-shim-mt6765/real/`),
BuildID `0bc3c35a2ffd85ff39c768deb4b0230b`.

### The e-ink path is exported and PLT-callable

```
DispDevice::triggerEinkCommit(CommitInfo*)        0xf52a0
  -> mtk_commit(eink_update_config)               bl ...@plt  (0xf5738, 0xf5984)
       -> ioctl(fd, 0x40444FE4, &cfg)             0xef900  (pure pass-through)
```

`0x40444FE4` = `_IOW(0x4f, 0xe4, 68)`. Confirmed as an e-ink family ioctl by the
neighbours in the same library: `0x40044FE9` (`_IOW(0x4f,0xe9,4)`, panel type in
`mtk_init`) and `0x40184FD5` (`_IOR(0x4f,0xd5,24)`, `mtk_waitVSync`).

Because `mtk_commit` is reached through the PLT (not `-Bsymbolic`), its GOT slot
is rewritable at runtime: userspace partial updates are possible.

### `eink_update_config` is 68 bytes, mode first

Recovered from three call sites in `triggerEinkCommit` and cross-checked against
the `DbgLogger` format strings (the 68-byte `sizeof` is also the ioctl size):

```
+0x00 u32 mode      +0x04 left  +0x08 top  +0x0c right  +0x10 bottom (inclusive)
+0x14 u32 dither    +0x18 u32[4] zero
+0x28 s32 fd        +0x2c u32 format
+0x30 u32[2] zero   +0x38 u32 stride  +0x3c u32 height  +0x40 u32 = 1
```

**This is not the B1051C structure.** B1051C is `drm_eink_update` with the rect
first and ioctl `0xC04464CF`; ours has `mode` first and `_IOW(0x4f,0xe4,68)`.
That alone makes the prebuilt module in `build/hwc-shim/` unusable, even with the
`mt6877 -> mt6765` string patch.

The rectangle is in the **1440x720 controller buffer**, not the 720x1440 SF
display (stock's own log: `update_area=(0,0,1439,719)`).

### Useful exports for later

`CommitPolicy::{prepareDirtyRegion,checkLayersAndSetCommitInfo,isWholeLayerDirty,
countFlushLimit,needCleanScreen}`, `OverlayEngine::{processImage,
convertLayerToGray,triggerEink,loopHandlerEink}`, `HWCDisplay::updateFps`,
`mtk_waitVSync`, `mtk_set_anti_flicker`, `mtk_set_eink_animation`,
`isDrmCommitEnable`, and the props `vendor.xrz.disable_rect_commit`,
`vendor.xrz.capture_commit`, `vendor.xrz.hwc.show_commit_fps`.

Note `[rect commit] total count=%zu` + `failed! dirty rect is too many`: the
vendor supports up to 10 rect commits per frame natively. The machinery is all
there; on the GSI the *inputs* degenerate to the full panel.

### New shim: `build/hwc-shim-mt6765/`

Written from scratch for this library (the B1051C source is far too specific to
port). Same two-layer design: HWC2 `getFunction` patch (capture damage from
`setClientTarget` / `setLayerSurfaceDamage`) + GOT hook on `mtk_commit`
(rewrite rect, optionally mode). Damage is accumulated between commits, so an
async commit lag can over-invalidate but never miss a pixel.

Built and clean (20 832 B, `NEEDED` = liblog/libdl/libm/libc only) with
`pwsh -File build.ps1` on the Windows host, NDK 28.2:
`aarch64-linux-android30-clang++ -shared -fPIC -O2 -nostdlib++ ... -llog -ldl`.

Deploy is reversible by reboot: shim and the staged real library live in
`/system/xbin` (tmpfs), the shim is bind-mounted over
`/vendor/lib64/hw/hwcomposer.mt6765.so`, then `setprop ctl.restart
vendor.hwcomposer-2-1`.

First run is **observe only** (`rect=0`): it logs, per commit, the real interval
(`dt`), the vendor rect/mode, and the damage rectangles SF supplied. That single
log answers the two open questions:

1. Does SF hand over a usable partial damage rect at all (`ct=` / `layer=`)?
2. Which transform maps display space to the 1440x720 controller space
   (`xform` 0..5)?

Only then flip `rect=1`. If SF's damage is always full, the fallback is a pixel
diff of the client target (needs the gralloc ion fd; not implemented yet).

Status: **built, not yet run on the phone** (phone was disconnected this
session; `adb devices` showed only the SM-T976B).

## HWC shim on device: measurements (2026-11-20 22:20)

`build/hwc-shim-mt6765/` was built, installed and measured on the TD A16 GSI.
It loads (as `hwcomposer.mtk_common.so`, wrapping the untouched real composer),
hooks `mtk_commit` via the GOT and can rewrite rect/mode/flag/height and the
present fence.

### Installation facts

* The composer (`hal_graphics_composer_default`) may only dlopen from `/odm`,
  `/vendor`, `/system/vendor` (ld.config.txt `[vendor]`), so `/system/xbin` and
  `/data` are impossible. `/vendor` is ro, so `/vendor/lib64/hw` (7.5 MB, 46
  files) is copied to tmpfs and bind-mounted back.
* Per-file SELinux labels must be preserved: `gralloc.rogue.so`,
  `gralloc.default.so`, `android.hardware.graphics.mapper@4.0-impl-mediatek.so`
  and `vulkan.mt6765.so` are `same_process_hal_file`. A blanket
  `chcon -R vendor_file` makes SF abort at startup with
  `no suitable EGLConfig found` (SkiaGLRenderEngine::chooseEglConfig) and the
  framework never returns.
* Restarting only the composer leaves the Java side with stale SurfaceControls:
  `system_server` dies ~90 s later (`InputChannel is not initialized`,
  `mNativeObject of Surface ... is null`) and wireless adb goes with it.
  Restart the composer and then `ctl.restart zygote`; adbd is untouched.
* `persist.adb.tcp.port=5555` makes adb survive reboots (wireless debugging
  itself turns off on reboot on this build).

### What the composer does

```
commit #4 dt=42422us ioctl=2606us pass mode=0x11 area=(0,0,1439,719)
           ct=(0,0,-1,-1)/0 layer=(0,0,-1,-1)/0
```

* full panel, Hyper (0x11), every frame.
* `setClientTarget` damage is always empty; `setLayerSurfaceDamage` is almost
  always the whole layer, occasionally a real small rect (`(6,74,34,102)`).
  Layer frames are already in the 1440x720 controller space (no transform).
* the ioctl returns in ~2.6 ms: the panel update is asynchronous and the ~40 ms
  is spent waiting afterwards.

### Measurements (all with continuous scrolling, n≈75-90 per point)

| change | median commit interval | rate |
|---|---|---|
| baseline | 45-46 ms | ~22 fps |
| rect = 1440x200 / 1440x40 / 200x200 | 42-45 ms | no gain |
| `height` = 200 / 40 | 35 ms | (see below, at the time nofence was on) |
| `flag` = 0 | 45.9 ms | no gain |
| mode 0x11 / 0x10 A2 / 0xb2 Fast / 0xb3 Extreme | 35.4 ms each | no difference |
| `fps_high`/`fps_low` = 60 | 35.7 ms | no gain |
| **present fence dropped (`nofence=1`)** | **35.4 ms** | **~28 fps** |

**Consequences**

1. Partial rectangles do not make this controller faster: a 40-line commit costs
   the same as a full one. The B1051C premise (shorter rect = shorter scan) does
   not hold on the MT6765 panel.
2. The one real win is not handing SurfaceFlinger the retire fence: SF stops
   waiting for each panel update before rendering the next frame. 22 -> 28 fps,
   which is the same ceiling the `debug.sf.*backpressure*` props reached earlier,
   so ~35 ms per update is a hard floor.
3. Stock's 66 commits/s cannot be partial-rect speed (proved above) and is not a
   waveform difference (all modes identical). It must be a different pipeline in
   Bigme's composer/SF (async, coalesced commits) that userspace cannot reach
   from the HWC interface.
4. Everything is reversible: `adb reboot`, or `umount -l /vendor/lib64/hw`.

---

## 2026-11-23 — why the GSI is slow: the vendor converts to gray, and it has a skip switch

Follow-up to `build/hwc-shim-mt6765/FINDING-cpu-bound.md`.

### The conversion is gated by one field, and the field is the format

Disassembly of `hwcomposer.mt6765.so` (BuildID 0bc3c35a2ffd85ff39c768deb4b0230b):

```
OverlayEngine::convertLayerToGray(param, ci)        0xa1260
  w27 = *(u32*)(ci + 0x54)                          0xa12c4     format
  ... ION import + mmap + sync_wait on param+0x12c (the CT fence)
  cbnz w27, 0xa14c4                                 0xa1488     format != 0 -> SKIP
  bl xrz::EffectHandler::convertToGray(dst=dst, src=dst, rect, pitch=height*4, 1)
                                                    0xa14c0     IN PLACE, on the CT

ComposerHandler::process(job)                       0xae668
  *(u32*)(ci + 0x54) = *(u32*)(job + 0x8)           0xae7f8

DispDevice::triggerEinkCommit(ci)                   0xf52a0
  cfg.format = *(u32*)(ci + 0x54)
  cfg.fd     = graphicbuffer->fd
```

So `format == 0` means "the buffer is gray8, convert it on the CPU (in place)";
`format != 0` means "the buffer is RGBA, the kernel converts it". The GSI's job
carries 0. That single 32-bit field is the 32 ms of one core per frame, and the
32 ms is why 22-28 fps was the ceiling in every earlier experiment.

`processImage` is called through the PLT (`bl 0x119770`, JUMP_SLOT at 0x1233f8),
so the existing GOT hook can write that field *before* the pipeline runs:

```
vendor.debug.hwc.shim.skipconv=1        -> ci+0x54 = 1
vendor.debug.hwc.shim.skipconv_fmt=N    -> any non-zero value
```

The offset is BuildID-specific and is gated on the BuildID being parsed out of
the loaded library (the same mistake the B1051C review found is not repeated).

### Second, independent route: own the commit

The worker is asynchronous: `presentDisplay` returns, then ~32 ms later the
worker calls `mtk_commit`. So the shim can commit from `presentDisplay` with the
RGBA client target (fd + fence + gralloc geometry, all already in hand), drop the
worker's commits, and (with `vend_hz`) stretch its vsync pacing. Knob `fast=1`;
`inplace=1` converts to gray8 in place and commits `format 0` if the kernel does
not convert RGBA. Details and test order: `FINDING-fast-commit.md`.

### What this changes about the earlier conclusions

* Partial rectangles do not help **because the commit interval was conversion
  time**, not scan time. Once the conversion is gone, rectangles and the diff
  become relevant again (and `fast_diff=1` makes a static screen commit nothing).
* Stock's 66 commits/s no longer needs an unreachable Bigme SF: stock's SF
  supplies the job with a non-zero format (or a gray buffer), so the CPU
  conversion is skipped. Same library, different caller.
* The next device run decides the one remaining unknown: whether this kernel's
  `0x40444FE4` path converts RGBA like B1051C's driver. The observe log's
  `fmt=` and the `skipconv_fmt` sweep answer it in minutes.

### 2026-11-23 23:10 — device results (TD A16 GSI, MT6765)

Measured with the bench APK (`com.hibreak.einkbench/.AnimActivity`):

| configuration | processImage | convertToGray | commit dt |
|---|---|---|---|
| vendor baseline | 37.2 ms | (inside) | 34 ms |
| our NEON converter, vendor copy | 35 ms | 2.3 ms | 34 ms |
| **fastconv=1 + skipcopy=1** | **3–4 ms** | **2.3–2.6 ms** | **~30 ms** |
| skipconv=1 (format=1, kernel converts) | 0.04 ms | n/a | 235 ms (ioctl 13 ms) |
| fastgray=1 (in-place gray on the CT) | 0.04 ms | 90 ms | 268 ms |

The 32 ms was a **redundant 4 MB memcpy**, not the conversion:

```
memcpy 31780us n=4147200 dst=0x74e187a000 src=0x74e1c6f000   (avg 33.5 ms)
convertToGray src=<CT> dst=<commit buffer> rect=(0,0,1440,720) stride=5760 bpp=1
```

`GraphicBuffer::lock/unlock` are 50–115 us, `sync_wait` ~30 us, `memcmp` of the
4.1 MB frame 24–40 us. So the copy is the whole 32 ms, and it is redundant: the
converter overwrites the same destination afterwards.

Our converter (NEON, ordered 4×4 dither, no divisions) was verified against the
vendor's on-device: pitch 1440, mean absolute difference **0.35/255**, max 29,
zero pixels off by more than 32. That is dither noise, not a layout error.

**Rejected:** handing RGBA to the kernel. `skipconv=1` makes processImage 40 us
(the vendor skips copy and conversion) but each commit then costs 13 ms of ioctl
and the panel interval 235 ms. The kernel's RGB→gray is worse than the
vendor's userspace one. `fastgray=1` (convert in place on the client target)
costs 90 ms because writing into the uncached dma-buf is slow.

**Remaining:** with CPU cost gone the interval is ~30 ms even for an 80-row
rectangle, so the next limiter is pacing (vendor `mtk_waitVSync` / panel), not
scan time or rectangles. `novsync=1` is being tested; if it unblocks the
cadence, partial rectangles become meaningful again for the first time.

### 2026-11-23 23:12 — pacing confirmed (preliminary)

With `fastconv=1 skipcopy=1 novsync=1` and a forced 80-row rectangle
(`test_h=80`):

```
commit #1805 dt=17147us pre=3841us ioctl=1684us RECT area=(0,0,1439,79) ret=0
commit #1825 dt=16515us pre=3883us ioctl=1726us RECT area=(0,0,1439,79) ret=0
...
dt 15.9-17.1 ms  ->  ~60 commits/s
```

So the ~30 ms floor was the vendor's `mtk_waitVSync` pacing, not the panel and
not the rectangle: bypassing the wait doubles the cadence to 60 Hz, `ioctl` stays
1.7 ms, and the driver keeps returning 0. The full-panel case with `novsync=1`
is the next measurement; if it stays at ~30 ms then the panel scan is the limit
for full frames and partial updates are the way past it, if it also drops to
16 ms then full-screen 60 Hz is already there.

### 2026-11-23 23:18 — full-panel 60 Hz per commit, 33/s average

With `fastconv=1 skipcopy=1 novsync=1`, full panel, bench animation:

```
commit #4045 dt=16612us pre=7186us ioctl=3281us pass area=(0,0,1439,719) ret=0
commit #4105 dt=12495us pre=4112us ioctl=2741us pass area=(0,0,1439,719) ret=0
```

- per-commit interval **12.5-17.4 ms** (60-80 Hz), `ret=0`, ioctl 2.6-3.3 ms;
- **332 commits in 10.0 s = 33.2/s** on the animating bench (baseline 26.7/s);
- commits arrive in bursts at 16 ms separated by gaps, so the *average* is lower
  than the per-commit rate. The gaps are not composer CPU any more
  (processImage 4 ms): either the app/SF only produces ~33 frames/s, or the
  vendor's "no dirty region" policy is skipping. That is the next measurement
  (SF present rate vs vendor skip count).

Conclusion so far: the composer is no longer the bottleneck. It costs 3-4 ms of
CPU per frame and can commit at 60 Hz; what remains is frame *supply* (SF/app
pacing), which is above the HWC interface.

### 2026-11-24 00:05 — CORRECTION: `skipcopy` is unsafe, the copy is required

Two mistakes in the entries above:

1. The commit counts were taken from log *lines* without multiplying by
   `log_every`, and the app's own frame counter was not measured. With
   `log_every=1` and `dumpsys gfxinfo` in the same window, the numbers separate.
2. `skipcopy=1` removes the 4 MB `memcpy` (processImage 35 ms -> 3-4 ms) but the
   vendor's frame-difference bookkeeping needs that copy. With it skipped the app
   renders at **62 fps** while the composer commits **~3/s** — change detection
   is defeated and the panel goes stale. It must not be used.

Measured on a fresh boot, 8 s windows, bench focused and awake:

| knobs | commits | app frames |
|---|---|---|
| all off (vendor) | 68 (8.5/s) | 28 fps |
| `fastconv=1` | 118 (14.8/s) | 28 fps |
| `fastconv=1 skipcopy=1` | ~25 (~3/s) | 62 fps |
| `fastconv=1 novsync=1` | 29 (3.6/s) | 28 fps |

`dt` stays ~35 ms and `pre` ~31 ms in every safe configuration: the interval is
the copy. Our converter is correct (verified 0.35/255) and cheaper, but it was
never the main cost — the vendor's converter is comparable; the 33 ms was always
the `memcpy`.

What is genuinely proven: the composer can commit at 12.5-17.4 ms per full frame
(`dt`, `ret=0`) when the copy is out of the way; the driver takes it. The only
safe way to get that is to stop using the vendor's staging buffer at all — the
`fast=1` path with **our own gray buffer** (ION/gralloc allocation), not the
in-place client-target conversion (90 ms of uncached writes).

Current safe state: `fastconv=1`, `skipcopy=0`, `novsync=0` — vendor semantics,
our converter, no behaviour change.

### 2026-11-24 00:35 — private-buffer own-commit path works; last blocker is buffer cacheability

Built `fast=1 ownbuf=1`: the shim allocates three 1440x720 gray buffers through
the GSI's `libion.so` (`ion_open`/`ion_alloc_fd`; the raw `ION_IOC_ALLOC` ioctl
returns ENOTTY on this kernel), converts the client target into them, and commits
`format=0` itself:

```
fast #374 own-gray [0,0,1439,719] mode=0xb2 fmt=0 stride=1440 h=720
           ioctl=2549us wait=7us sync=718us ret=0
ownbuf 78122us (n=760 avg=79245us) 1440x720 -> fd=109
```

So the pipeline is complete and the vendor is fully bypassed (vendor commits 0).
Per-frame work that is fast: fence wait 7 us, dma-buf sync 0.7 ms, converter
2.5 ms, ioctl 2.5 ms. The one slow part is the **write into the ION buffer:
~78 ms for 1 MB** — the mapping is uncached (~13 MB/s), while the vendor's
gralloc commit buffer takes the same 1 MB write in ~2 ms.

Next step is therefore allocation/caching, not the pipeline: allocate the gray
buffer through gralloc (`GraphicBufferAllocator`/`GraphicBufferMapper` from
libui) so its CPU mapping is cached like the vendor's, or find the cached ION
heap/flags (`ob_heap`, `ob_flags` knobs are in place; MTK cached ION may need
`ION_FLAG_CACHED|CACHED_NEEDS_SYNC`).

Also learned: `presentDisplay` is not called per frame — SF reuses the client
target, so the fast path must commit on every present with a valid snapshot
(no "new client target" sequence guard).

### 2026-11-24 — the HiBreak Pro LineageOS repo (eckelhesten/BigmeHibreakPro-LineageOS)

Same vendor e-ink stack (`xrz`) on MT6877, Android 16, with the source patches.
Directly useful, and it explains two things we hit blind:

1. **Stock props the GSI is missing** (`configs/props/system.prop` +
   `init.hibreakpro.eink.rc`):
   - `vendor.xrz.disable_eink_vsync 1` — stock disables the e-ink vsync wait; we
     had been bypassing it with the `novsync` GOT hook.
   - `vendor.xrz.hwc_idle 1`, `vendor.xrz.auto_clean_enabled 1`,
     `vendor.xrz.anti_flicker_enabled 1`.
   `deploy.ps1` now sets these on every install.
2. **`/sys/kernel/debug/eink_debug/clean_a2`** gates the kernel's fast
   whole-panel DU/A2 path: "only with it on do whole-panel DU/A2 updates run on
   the kernel's fast path (10-frame DU, ~120 ms per frame, pipelined) ... Off,
   the same updates take ~250 ms each." A GSI never writes it; `apply.sh` now
   does (along with `anti_flicker`), guarded by the node's existence.
3. **Damage semantics**: HWC2 encodes "whole layer changed" as zero rects, and
   the vendor CommitPolicy reads zero rects as "no dirty region" and *skips the
   commit*. That matches the `layer=(0,0,-1,-1)/0` frames where our vendor-path
   commit rate collapsed. Stock SF patches add `debug.sf.hwc_explicit_full_damage`
   to send explicit bounds.
4. **Stock's fast refresh is SF-side**: their patch 0013 renders the frame in
   SurfaceFlinger (GPU) already dithered to 1-bit/16-level grey, so the composer
   only copies it. A prebuilt GSI cannot take that patch — which is exactly why
   the own-commit path with our own cached grey buffer is the right equivalent.
5. Do **not** copy their mode numbers: MT6877 policy modes are `0xB3` FAST /
   `0xB0` CLEAN, while our mt6765 library uses `0xb1` HD, `0xb2` Fast, `0xb3`
   Extreme, `0x11` Hyper. The encodings differ.

### 2026-11-24 06:30 — SOLVED: 62-64 fps on the TD A16 GSI

Final configuration: **`fastconv=1 skipcopy=1`** (vendor pipeline, vendor's own
cached commit buffer) **plus the stock e-ink properties and `clean_a2`** from the
HiBreak Pro device tree:

```
vendor.xrz.disable_eink_vsync   1
vendor.xrz.hwc_idle             1
vendor.xrz.auto_clean_enabled   1
vendor.xrz.anti_flicker_enabled 1
/sys/kernel/debug/eink_debug/clean_a2    1   (was 0 on the GSI)
/sys/kernel/debug/eink_debug/anti_flicker 1
```

Measured with the animating bench, full panel, `format=0`, `ret=0`:

| metric | value |
|---|---|
| vendor commits | 507-516 per 8 s = **63-64.5/s** |
| app frames | 493-495 per 8 s = **61.6-61.9 fps** |
| commit `dt` | 14.0-19.2 ms |
| `pre` (present->commit) | 4-5 ms |
| ioctl | 2.5-3.1 ms |
| static screen | 2 commits in 6 s |

So: ~2.3x the 26.7/s baseline, at stock-class 62-64 fps, on the full panel, with
the vendor's own gray8 buffer and waveform. The earlier "skipcopy collapses
commits" result was an artefact of the GSI's e-ink stack being un-initialised
(`clean_a2` off, no `vendor.xrz.*` props); with those set, the vendor's change
detection still works (static screen commits ~0.3/s).

Rejected / not needed: the private ION buffer (`ownbuf`) commits correctly
(`own-gray`, `fmt=0`, ioctl 2.7 ms) but every write into an ION mapping costs
~80 ms on this kernel even from `ion_system_heap` with
`ION_FLAG_CACHED|CACHED_NEEDS_SYNC`; the vendor's gralloc buffer is the only fast
destination. RGBA to the kernel remains rejected (13-235 ms).

### 2026-11-24 06:50 — CORRECTION: that 63/s was black frames

The 63-64 commits/s figure above is real, but the **image was black**. `skipcopy`
does not remove a redundant staging pass: it removes the *input* to the vendor's
converter (the staging buffer that `convertToGray` reads). With it skipped, the
converter converts an empty buffer, and the panel gets 63 black frames per second.

Why the fast alternative failed too:

- raw `mmap` of the client target dma-buf is **uncached** on this kernel:
  reading 1440x720 RGBA costs ~80 ms (13 MB/s), the same as writing;
- `ion_import` of that fd fails with `ENOTTY`, so ION's cached mapping path
  cannot be used on the client target;
- the vendor's converter is fast only because it uses `GraphicBuffer::lock` /
  `IONDevice::ionMMap` on **its own** buffers.

Correct state, and what it measures: `fastconv=1 skipcopy=0` — our converter
(2.3 ms, verified 0.35/255) replaces the vendor's, the vendor's 4 MB staging copy
stays, interval ~34-37 ms, i.e. **~28 fps, correct image**.

Conclusion about 60 fps on this GSI: the ceiling is the vendor's 4 MB RGBA
staging copy, because the GSI's prebuilt SurfaceFlinger hands the composer RGBA.
Stock is fast because Bigme's/the Pro project's SurfaceFlinger converts to
1-bit/16-level grey on the GPU first (patch 0013), so the composer moves 1 MB.
Reaching 60 fps therefore needs the SF-side e-ink render pass — a source build —
not a userspace HWC shim. Everything else in this log (converter, pacing,
properties, kernel knobs) is correct and useful, but it cannot remove that copy.

> **Correction 2026-11-24 16:10:** the “composer moves 1 MB” claim above is not
> supported by the published Pro patches. Patch `0013` keeps the client target
> RGBA_8888; it pre-quantizes pixel values but does not compact the buffer. See
> `SF-COMPATIBILITY-AUDIT.md` for the source/binary proof and rebased patch.

### 2026-11-24 08:45 — the missing e-ink init, fixed persistently

The single biggest quality fix was not the shim: the GSI never initialises the
kernel's e-ink stack. Setting the stock properties and writing the debugfs nodes

    setprop vendor.xrz.auto_clean_enabled 1
    setprop vendor.xrz.anti_flicker_enabled 1
    setprop vendor.xrz.hwc_idle 1
    echo 1 > /sys/kernel/debug/eink_debug/clean_a2
    echo 1 > /sys/kernel/debug/eink_debug/anti_flicker

"made a huge difference" on the phone (ghosting in the fast modes, Regal
stutter). `clean_a2` gates the kernel's whole-panel fast path and ghost cleanup;
it was **0** on the GSI because stock's `init.eink.rc` turns it on from the
property, and the GSI has no such handler.

E-ink Center now does it at boot: `BootReceiver` writes `clean_a2` and
`anti_flicker` directly (in addition to `auto_clean_enabled`), and no longer
throws away its tick alarms when `startForegroundService` is refused after boot.
Rebuilt with `build/eink-center/build-win.ps1` (SDK build-tools + the repo's
debug.keystore). Verified after a reboot with adb root:

    clean_a2=1  auto_clean=1  anti_flicker=1  hwc_idle=1

Modes (E-ink Center values): Fast 178 (default, 0xB2), A2 16, Extreme 179,
Hyper 183, HD 2147483825, Regal 180. Fast modes are 1-bit and need the cleanup
above; Regal is a slow high-quality waveform and will always stutter on motion.

### 2026-11-24 09:10 — `ctsrc` (mapper read of the client target) is unsafe in the composer

The mapper probe promised the cached read we needed: import the client target
with `GraphicBufferMapper::importBuffer` and `lock()` it (~2.2 ms for 4 MB, vs
80 ms for a raw mmap). Wiring that into `convertToGray` (`skipcopy=1 ctsrc=1`)
**crashes the composer**: tombstone_12, `Dispatcher_0`, SIGSEGV in
`gralloc.rogue.so`, fault addr 0x8 (null member deref). That is the same fault
the probe's own comment recorded for locking a handle this process did not
allocate; importing did not make it safe here.

Also: `deploy.ps1` sets its knobs through `adb shell` and ignores failures. While
the wireless link was flapping, those setprops silently failed, the old
`ctsrc=1 skipcopy=1` stayed set, and the next apply.sh restarted the composer
into the crashing configuration. Any future deploy should read the knobs back.

Decision: stop the shim experiments on this phone. Leave the GSI on the plain
vendor path plus the persistent E-ink Center fix (`clean_a2`, `anti_flicker`,
the `vendor.xrz.*` properties) — that is the quality win and it survives
reboots. The shim source is repaired and builds (`-Fastconv 1` is the safe
mode), and `ctsrc`/`ctprobe` remain as documented dead ends.










