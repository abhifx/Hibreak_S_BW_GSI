// hwcshim_mt6765 -- HWC2 interposer for Bigme HiBreak (MT6765) hwcomposer.mt6765.so
//
// Goal: partial e-ink updates on the TrebleDroid A16 GSI without a source build.
//
// WHY THIS LAYER (reverse engineering 2026-11-20, library BuildID 0bc3c35a2ffd85ff39c768deb4b0230b)
//
//   The mt6765 vendor composer is not the mt6877/drm_eink_update design used by
//   the B1051C shim. It exports its own e-ink path:
//
//     DispDevice::triggerEinkCommit(CommitInfo*)          @ 0xf52a0
//       -> mtk_commit(eink_update_config)  (PLT call @ 0xf5738 / 0xf5984)
//            -> ioctl(eink_fd, 0x40444FE4, cfg)          @ 0xef900
//
//   0x40444FE4 = _IOW(0x4f, 0xe4, 68): "eink_update_config", 68 bytes, passed
//   by value (AAPCS64: by reference to a caller stack copy, so a GOT hook can
//   rewrite it in place). Layout, confirmed against three independent call
//   sites (normal commit, per-rect commit, rect loop):
//
//     +0x00 u32 mode      (0xb1 HD, 0xb2 Fast, 0xb3 Extreme, 0x11 Hyper, 0x10 A2,
//                          0x800000b1 HD256)
//     +0x04 u32 left      +0x08 u32 top      +0x0c u32 right  +0x10 u32 bottom
//                         (inclusive, in the 1440x720 e-ink buffer, not the
//                          720x1440 SurfaceFlinger display)
//     +0x14 u32 dither
//     +0x18 u32[4] zero
//     +0x28 s32 fd        +0x2c u32 format
//     +0x30 u32[2] zero
//     +0x38 u32 stride    +0x3c u32 height
//     +0x40 u32 = 1
//
//   The vendor recomputes the update rectangle itself from the layers' damage
//   (CommitPolicy::prepareDirtyRegion / isWholeLayerDirty). On the GSI it
//   degenerates to the full panel every frame (logged
//   "[normal commit]: mode=..,update_area=(0,0,1439,719)"), which costs one
//   full 1440-line scan (~42-47 ms) per frame -> ~24 fps.
//
//   This shim therefore does NOT rely on the vendor's rectangle. It watches the
//   damage SurfaceFlinger hands over (ABI defined) and rewrites the rectangle
//   (and optionally the mode) of every e-ink commit.
//
// TWO LAYERS OF VISIBILITY (same design as the B1051C shim)
//
//   IN  - HWC2 calls: we patch getFunction on the hwc2_device the real HWC
//         returns and hand that same device back, so every real entry point
//         still receives the pointer it expects.
//   OUT - the real HWC calls its own exported mtk_commit through the PLT (it is
//         not linked -Bsymbolic), so we rewrite that GOT slot.
//
// Damage accounting: rects are ACCUMULATED between commits and cleared when a
// commit consumes them. The vendor's commit runs on the e-ink worker thread and
// can lag SurfaceFlinger by a frame; a union over the interval can over-invalidate
// but can never miss a changed pixel.
//
// Runtime knobs (vendor.debug.hwc.shim.*, readable from the composer domain;
// set them, then restart the composer so they are re-read):
//
//   log         0 quiet, 1 normal (default), 2 verbose per-frame
//   disable     1 = pure passthrough (no hooks at all)
//   rect        0 = observe only (DEFAULT), 1 = rewrite rect from damage
//   mode        -1 = keep vendor mode (default), else force 0x11/Hyper or 0x10/A2
//   src         1 = client-target damage (default), 2 = layer-damage union
//   xform       how to map display space to e-ink buffer space (log first!)
//               0 identity (default), 1 swapxy, 2 swapxy+mirror x, 3 swapxy+mirror y,
//               4 rot180, 5 rot180+swapxy
//   min_pct     only rewrite when the new rect is smaller than the vendor rect
//               (1, default) or always (0)
//   log_every   log one commit summary every N frames (default 60)
//   skipconv    1 = force CommitInfo+0x54 non-zero, so the vendor skips its own
//               RGBA->gray conversion and the kernel does it (the 32 ms fix)
//   fast        1 = commit from presentDisplay ourselves (own the commit)
//   inplace     1 = RGBA->gray8 in place, commit format 0 (kernel cannot convert)
//   fast_diff   1 = pixel-diff the client target and commit only what changed
//   fast_hz     >0 = max fast commits/s,  vend_hz >0 = stretch the vendor worker
//
// Build: see build.ps1. Deploy: see deploy.sh. Revert: reboot.

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#if __has_include(<linux/dma-buf.h>)
#include <linux/dma-buf.h>
#define HAVE_DMABUF_SYNC 1
#else
#define HAVE_DMABUF_SYNC 0
#endif

#define TAG "hwcshim"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGV(...) do { if (g_log >= 2) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__); } while (0)

// The real library. The composer's linker namespace permits only /odm, /vendor and
// /system/vendor (verified on this device: ld.config.txt [vendor]
// namespace.default.permitted.paths), so the real library must be reachable from
// one of those. /vendor/lib64/hw is the natural place, and there are two ways to
// arrange it (see deploy.ps1):
//
//   "variant"  shim installed as hwcomposer.mtk_common.so (ro.hardware.hwcomposer),
//              real library left untouched as hwcomposer.mt6765.so.
//   "replace"  shim installed as hwcomposer.mt6765.so, real library renamed to
//              hwcomposer.mt6765.real.so.
//
// Try them in order; the recursion case (replace: mt6765.so is *us*) is rejected
// by comparing the file identity against our own module.
static const char* REAL_HWC_CANDIDATES[] = {
    "/vendor/lib64/hw/hwcomposer.mt6765.real.so",   // "replace" layout
    "/vendor/lib64/hw/hwcomposer.mt6765.so",        // "variant" layout
    "/system/xbin/hwcomposer.mt6765.real.so",       // not in the namespace: last resort
    "/data/local/tmp/hwcomposer.mt6765.real.so",
};
static const char* REAL_HWC = REAL_HWC_CANDIDATES[0];
// Matches both hwcomposer.mt6765.real.so and hwcomposer.mt6765.so; a library that
// does not call mtk_commit simply yields no hits.
static const char* REAL_HWC_MATCH = "hwcomposer.mt6765";

// ---------------------------------------------------------------- libhardware
// Minimal LP64 copies of hardware/libhardware/include/hardware/{hardware,hwcomposer2}.h
// (the NDK does not ship them).

struct hw_module_t;
struct hw_device_t;
struct hw_module_methods_t {
    int (*open)(const hw_module_t*, const char*, hw_device_t**);
};
struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char* id;
    const char* name;
    const char* author;
    hw_module_methods_t* methods;
    void* dso;
    uint64_t reserved[32 - 7];
};
struct hw_device_t {
    uint32_t tag;
    uint32_t version;
    hw_module_t* module;
    uint64_t reserved[12];
    int (*close)(hw_device_t*);
};
#define HWC_TAG(A, B, C, D) (((A) << 24) | ((B) << 16) | ((C) << 8) | (D))
#define HARDWARE_MODULE_TAG HWC_TAG('H', 'W', 'M', 'T')

typedef void (*hwc2_function_pointer_t)();
struct hwc2_device_t {
    hw_device_t common;
    void (*getCapabilities)(hwc2_device_t*, uint32_t* outCount, int32_t* outCapabilities);
    hwc2_function_pointer_t (*getFunction)(hwc2_device_t*, int32_t descriptor);
};

typedef uint64_t hwc2_display_t;
typedef uint64_t hwc2_layer_t;
struct native_handle_t { int version; int numFds; int numInts; int data[0]; };
// The NDK does not expose libcutils. These handles are plain malloc-backed
// metadata in AOSP, so clone/free them locally instead of adding a VNDK-only
// link dependency to the shim.
static native_handle_t* native_handle_clone(const native_handle_t* handle) {
    if (!handle || handle->numFds <= 0 || handle->numFds > 128 ||
        handle->numInts < 0 || handle->numInts > 4096) return nullptr;
    const size_t bytes = sizeof(native_handle_t) +
                         (size_t)(handle->numFds + handle->numInts) * sizeof(int);
    native_handle_t* copy = static_cast<native_handle_t*>(malloc(bytes));
    if (!copy) return nullptr;
    memcpy(copy, handle, bytes);
    for (int i = 0; i < handle->numFds; ++i) {
        copy->data[i] = dup(handle->data[i]);
        if (copy->data[i] < 0) {
            for (int j = 0; j < i; ++j) close(copy->data[j]);
            free(copy);
            return nullptr;
        }
    }
    return copy;
}
static void native_handle_delete(native_handle_t* handle) {
    if (!handle) return;
    for (int i = 0; i < handle->numFds; ++i) close(handle->data[i]);
    free(handle);
}

// The vendor worker may outlive g_snap. A handle cloned while g_lock is held
// keeps the GB01 metadata and its fd references valid for the whole conversion.
struct NativeHandleLease {
    native_handle_t* handle = nullptr;
    ~NativeHandleLease() { native_handle_delete(handle); }
};
typedef const native_handle_t* buffer_handle_t;
struct hwc_rect_t { int32_t left, top, right, bottom; };
struct hwc_frect_t { float left, top, right, bottom; };
struct hwc_region_t { size_t numRects; const hwc_rect_t* rects; };

enum {
    FN_ACCEPT_DISPLAY_CHANGES = 1, FN_CREATE_LAYER = 2, FN_DESTROY_LAYER = 4,
    FN_DUMP = 6, FN_GET_CHANGED_COMPOSITION_TYPES = 8,
    FN_GET_DISPLAY_REQUESTS = 14, FN_PRESENT_DISPLAY = 20, FN_REGISTER_CALLBACK = 21,
    FN_SET_CLIENT_TARGET = 23, FN_SET_LAYER_BLEND_MODE = 27, FN_SET_LAYER_BUFFER = 28,
    FN_SET_LAYER_COMPOSITION_TYPE = 30, FN_SET_LAYER_DATASPACE = 31,
    FN_SET_LAYER_DISPLAY_FRAME = 32, FN_SET_LAYER_PLANE_ALPHA = 33,
    FN_SET_LAYER_SOURCE_CROP = 35, FN_SET_LAYER_SURFACE_DAMAGE = 36,
    FN_SET_LAYER_TRANSFORM = 37, FN_SET_LAYER_Z_ORDER = 39,
    FN_SET_POWER_MODE = 41, FN_VALIDATE_DISPLAY = 43,
    FN_MAX_PROBE = 128,
};

typedef int32_t (*PFN_acceptDisplayChanges)(hwc2_device_t*, hwc2_display_t);
typedef int32_t (*PFN_createLayer)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t*);
typedef int32_t (*PFN_destroyLayer)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t);
typedef void    (*PFN_dump)(hwc2_device_t*, uint32_t* outSize, char* outBuffer);
typedef int32_t (*PFN_getChangedCompositionTypes)(hwc2_device_t*, hwc2_display_t, uint32_t*, hwc2_layer_t*, int32_t*);
typedef int32_t (*PFN_getDisplayRequests)(hwc2_device_t*, hwc2_display_t, int32_t*, uint32_t*, hwc2_layer_t*, int32_t*);
typedef int32_t (*PFN_presentDisplay)(hwc2_device_t*, hwc2_display_t, int32_t* outFence);
typedef int32_t (*PFN_setClientTarget)(hwc2_device_t*, hwc2_display_t, buffer_handle_t, int32_t, int32_t, hwc_region_t);
typedef int32_t (*PFN_setLayerBlendMode)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, int32_t);
typedef int32_t (*PFN_setLayerBuffer)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, buffer_handle_t, int32_t);
typedef int32_t (*PFN_setLayerCompositionType)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, int32_t);
typedef int32_t (*PFN_setLayerDataspace)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, int32_t);
typedef int32_t (*PFN_setLayerDisplayFrame)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, hwc_rect_t);
typedef int32_t (*PFN_setLayerPlaneAlpha)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, float);
typedef int32_t (*PFN_setLayerSourceCrop)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, hwc_frect_t);
typedef int32_t (*PFN_setLayerSurfaceDamage)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, hwc_region_t);
typedef int32_t (*PFN_setLayerTransform)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, int32_t);
typedef int32_t (*PFN_setLayerZOrder)(hwc2_device_t*, hwc2_display_t, hwc2_layer_t, uint32_t);
typedef int32_t (*PFN_setPowerMode)(hwc2_device_t*, hwc2_display_t, int32_t);
typedef int32_t (*PFN_validateDisplay)(hwc2_device_t*, hwc2_display_t, uint32_t*, uint32_t*);

// ---------------------------------------------------------------- eink config

// The struct mtk_commit() forwards verbatim to ioctl(0x40444FE4). Offsets were
// recovered from DispDevice::triggerEinkCommit; see the file header.
struct eink_update_config {
    uint32_t mode;      // 0x00
    uint32_t left;      // 0x04
    uint32_t top;       // 0x08
    uint32_t right;     // 0x0c  inclusive
    uint32_t bottom;    // 0x10  inclusive
    uint32_t dither;    // 0x14
    uint32_t r18[4];    // 0x18
    int32_t  fd;        // 0x28
    uint32_t format;    // 0x2c
    uint32_t r30[2];    // 0x30
    uint32_t stride;    // 0x38
    uint32_t height;    // 0x3c
    uint32_t flag;      // 0x40  (vendor always writes 1; possibly "wait for completion")
};
static_assert(sizeof(eink_update_config) == 0x44, "eink_update_config must be 68 bytes");

static int (*real_mtk_commit)(eink_update_config*);   // captured from the real GOT slot
static int (*real_mtk_waitvsync)(int64_t* ts);       // exported: mtk_waitVSync(int64_t*)
// OverlayEngine's per-frame work: these are what the commit interval is spent in.
static int64_t (*real_process_image)(void*, void*, void*);
static int64_t (*real_convert_layer_gray)(void*, void*, void*);

struct CallStat { uint64_t n, sum_us, max_us; };
static CallStat g_st_pi, g_st_clg, g_st_gray, g_st_conv;
static CallStat g_st_sync, g_st_gblock, g_st_memcpy;
static uint64_t g_skipcopy_hits;

static int (*real_sync_wait)(int, int);
static int (*real_memcmp)(const void*, const void*, size_t);
static int (*real_gb_lock)(void*, uint32_t, void**, int32_t*, int32_t*);
static void* (*real_memcpy)(void*, const void*, size_t);

// Client target, as handed over by SurfaceFlinger. On this panel the client target
// is already in the controller's 1440x720 orientation (layer frames are too), so if
// the kernel converts RGBA->gray there is no rotation to do.
static int (*real_gralloc_extra_query)(buffer_handle_t, int32_t, void*);
static int g_use_ct = 0;          // 1 = commit the RGBA client target instead of the gray buffer
static int g_ct_format = -1;      // override the format written for the client target (-1 = as reported)
struct CtInfo {
    int fd, w, h, stride, vstride, format;
    bool valid;
};
static CtInfo g_ct = { -1, 0, 0, 0, 0, 0, false };

static void ct_query(buffer_handle_t h) {
    if (!h) return;
    CtInfo c = { -1, 0, 0, 0, 0, 0, false };
    if (real_gralloc_extra_query) {
        int32_t v = 0;
        if (real_gralloc_extra_query(h, 1, &v) == 0) c.fd = v;
        v = 0; if (real_gralloc_extra_query(h, 10, &v) == 0) c.w = v;
        v = 0; if (real_gralloc_extra_query(h, 11, &v) == 0) c.h = v;
        v = 0; if (real_gralloc_extra_query(h, 12, &v) == 0) c.stride = v;
        v = 0; if (real_gralloc_extra_query(h, 13, &v) == 0) c.vstride = v;
        v = 0; if (real_gralloc_extra_query(h, 15, &v) == 0) c.format = v;
    }
    if (c.fd < 0 && h->numFds > 0) c.fd = h->data[0];
    c.valid = (c.fd >= 0 && c.w > 0 && c.h > 0);
    if (c.valid) g_ct = c;
}
static inline void stat_add(CallStat* s, uint64_t us) {
    s->n++;
    s->sum_us += us;
    if (us > s->max_us) s->max_us = us;
}
static inline uint64_t stat_avg(const CallStat* s) { return s->n ? s->sum_us / s->n : 0; }

// ---------------------------------------------------------------- fast path (own the commit)
//
// MEASURED (FINDING-cpu-bound.md): SurfaceFlinger is ready to present ~54x/s, but
// the vendor's e-ink worker spends ~32 ms of one core between presentDisplay()
// returning and mtk_commit(): OverlayEngine::processImage / convertLayerToGray
// re-convert the already-composited client target to 16-level gray, and the
// rectangle is always the full panel. mtk_commit itself is 2.7 ms.
//
// The vendor worker is asynchronous (presentDisplay returns first), so the fix is
// not to make it faster -- it is to stop using it. In fast mode the shim commits
// from presentDisplay(), where it already has
//
//   * the composited client target (SF already produced the final frame because
//     this GSI is forced into client composition),
//   * the dma-buf fd, format and stride (gralloc_extra),
//   * the acquire fence (wait for it, then the buffer is stable),
//   * the layer damage (the only usable damage on this HWC 2.1 build),
//
// builds eink_update_config itself and calls mtk_commit directly. The vendor's
// worker is then made harmless: its mtk_commit calls are dropped, and (vend_hz)
// its vsync pacing is stretched so it stops burning a core on conversions whose
// result nobody uses.
//
// Like B1051C (commit_use_ct), the RGBA buffer is handed to the kernel with
// format != 0; that driver path converts to gray itself. If this kernel instead
// wants 8 bpp, `inplace` converts the client target to gray8 in place
// (allocation-free, forward-safe: byte i is written only after pixel i has been
// read) and commits it as format 0.
struct CtSnap {
    int fd;        // dup of the client target's dma-buf
    int fence;     // dup of its acquire fence (-1 = none)
    int w, h;
    int stride;    // pixels
    int vstride;
    int format;    // HAL pixel format as reported by gralloc_extra
    uint64_t usage; // gralloc usage (query 16 when available)
    bool valid;
    native_handle_t* handle = nullptr; // cloned metadata/fds for GB01 transfer
};
static CtSnap g_snap = { -1, -1, 0, 0, 0, 0, 0, 0, false };
static int g_snap_seq;                // incremented on every new client target
static int g_consumed_seq;            // last client target presentDisplay acted on

// The vendor worker is asynchronous, so "the latest client target" is not
// necessarily the one belonging to the CommitInfo currently being converted.
// Keep a small history keyed by dma-buf identity; the worker binds the exact
// snapshot named by OverlayPortParam+0x78 before it calls processImage.
struct SnapHist {
    CtSnap snap;
    uint64_t seq;
    uint64_t dev;
    uint64_t ino;
    bool valid;
};
static SnapHist g_snap_hist[8];

// Job-local binding for the worker thread. Everything here is owned by this
// thread for the duration of one processImage/convertLayerToGray call.
struct SnapJob {
    bool active;
    bool matched;
    bool copy_skipped;
    bool helper_done;
    bool helper_ambiguous;
    bool fence_waited;
    void* self;
    void* param;
    void* ci;
    int dst_fd;
    uint64_t src_dev;
    uint64_t src_ino;
    uint64_t dst_dev;
    uint64_t dst_ino;
    CtSnap snap;
    uint64_t seq;
    int damage_l = 0;
    int damage_t = 0;
    int damage_r = -1;
    int damage_b = -1;
    bool damage_valid = false;
};
static __thread SnapJob t_job = {
    false, false, false, false, false, false, nullptr, nullptr, nullptr, -1, 0, 0, 0, 0,
    { -1, -1, 0, 0, 0, 0, 0, 0, false }, 0,
};

static int g_fast;                    // 1 = commit from presentDisplay (own the commit)
static int g_fast_idle;               // 1 = no damage -> no commit at all
static int g_fast_hz;                 // >0 = never commit faster than this
static int g_fast_diff;               // 1 = pixel-diff the CT, use the changed bbox
static int g_fast_wait_ms;            // acquire-fence timeout
static int g_inplace;                 // 1 = RGBA->gray8 in place, commit format 0
static int g_inplace_dither;          // 1 = ordered dither to 16 levels
static int g_vend_hz;                 // >0 = stretch the vendor worker to this rate
static int g_dither = -1;             // -1 = mirror the vendor's last dither
static int g_skipconv;                // 1 = make the vendor skip its gray conversion
static int g_skipconv_fmt = 1;        // CommitInfo+0x54 value that means "caller is RGBA"
static int g_fastgray;                // 1 = skipconv + convert RGBA->gray8 ourselves in mtk_commit
static int g_fastconv;                // 1 = replace xrz convertToGray with our own converter
static int g_dither1 = 1;             // 1 = dither to black/white in the 1-bit modes
static int g_verify;                  // 1 = compare our converter with the vendor's (keeps vendor output)
static int g_conv_dst_pitch = -1;     // destination pitch in bytes (-1 = stride/4)
static int g_skipcopy;                // 1 = drop the vendor's redundant 4 MB frame copy
static int g_damagecopy;              // 1 = copy only CT-damaged pixels into the staging buffer
static int g_xproc;                   // 1 = use the out-of-process cached gralloc gray helper
static int g_xproc_vendor;            // 1 = helper feeds conversion, vendor performs commit
static int g_xproc_damage;            // 1 = convert only valid CT damage when safe
static int g_xproc_ready;             // atomic: helper path has succeeded at least once
static int g_helper_onebit;           // 1 = force B/W helper output; 0 = 16-level gray
static uint64_t g_damagecopy_hits, g_damagecopy_bytes, g_damagecopy_full_bytes;
static uint64_t g_xproc_partial, g_xproc_full, g_xproc_partial_pixels;
static CallStat g_st_xproc;
static uint64_t g_xproc_fails;
static int g_xproc_consec_fails;
static int g_xproc_attempts;
static uint64_t g_xproc_last_success_ns;
static uint64_t g_xproc_retry_after_ns;
static uint64_t g_helper_recovery_last_ns;
static bool g_helper_recovery_set;
struct DestQuarantine {
    uint64_t dev;
    uint64_t ino;
    uint64_t until_ns;
};
static DestQuarantine g_dest_quarantine[4];
static bool g_drop_next_commit;
static int g_xproc_sock = -1;
static int g_xproc_listen = -1;
static pthread_mutex_t g_xproc_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_vendor_commit_fds[4];
static int g_vendor_commit_fd_count;
static int g_vendor_commit_fd_next;
static constexpr uint32_t kRawOutputMagic = 0x314f5748; // 'HWO1'
struct GrayRequest {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t onebit;
    uint32_t reserved;
    uint32_t left;
    uint32_t top;
    uint32_t right;
    uint32_t bottom;
};
static_assert(sizeof(GrayRequest) == 40, "GrayRequest wire size changed");
static constexpr uint32_t kGrayMagic = 0x31475748;       // 'HWG1' little-endian
static constexpr uint32_t kGrayErrorMagic = 0x31455748;  // 'HWE1' little-endian
static constexpr uint32_t kGbMagic = 0x47423031;         // AArch64 'GB01' multi-character literal
static int g_ctsrc;                   // 1 = convert from the client target, not the staging buffer
static int g_ownbuf;                  // 1 = fast path commits our own ION gray buffer
static int g_ctprobe;                 // 1 = time the mapper against the raw mmap (read-only)
static int g_vendor_fd_probe;         // 1 = one-time raw mmap/read probe of vendor commit fds
static int g_ob_heap = -1;            // heap mask for libion (-1 = 0xffffffff)
static int g_ob_flags = -1;           // ION flags (-1 = try CACHED then 0)
static uint64_t g_last_wait_us, g_last_sync_us;
static int (*p_ion_open)(void);
static int (*p_ion_alloc_fd)(int, size_t, size_t, unsigned, unsigned, int*);
static int (*p_ion_heap_cnt)(int, int*);
static int (*p_ion_get_heaps)(int, int, void*);
static int (*p_ion_import)(int, int*);
static int (*p_ion_map)(int, int, size_t, int, int, off_t, unsigned char**, int*);
static int (*p_ion_free)(int, int);
static int g_ion_fd = -1;

// AOSP libion: struct ion_heap_data. Used to find ion_system_heap's id so the
// allocation does not land on the media heap (which ignores ION_FLAG_CACHED).
struct ion_heap_data {
    char name[32];
    unsigned int type;
    unsigned int heap_id;
    unsigned int reserved0, reserved1, reserved2;
};

// Private gray buffers for the own-commit path. Triple-buffered: the driver may
// still be scanning one while we convert into the next.
// ION uapi (kernel 4.19; the NDK does not ship linux/ion.h)
struct ion_allocation_data {
    uint64_t len;
    uint32_t align;
    uint32_t heap_id_mask;
    uint32_t flags;
    int32_t  handle;
};
struct ion_fd_data { int32_t handle; int32_t fd; };
#define ION_IOC_MAGIC 'I'
#define ION_IOC_ALLOC _IOWR(ION_IOC_MAGIC, 0, struct ion_allocation_data)
#define ION_IOC_MAP   _IOWR(ION_IOC_MAGIC, 2, struct ion_fd_data)
#define ION_FLAG_CACHED 1
#define ION_FLAG_CACHED_NEEDS_SYNC 2

struct OwnBuf { int fd; uint8_t* map; size_t len; int w, h; };
static OwnBuf g_ob[3];
static int g_ob_count, g_ob_next;
static CallStat g_st_own;

static bool g_ion_loaded;

static void load_ion(void) {
    if (g_ion_loaded) return;
    g_ion_loaded = true;
    void* h = dlopen("libion.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("/system/lib64/libion.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) { LOGE("ion: libion not loadable"); return; }
    *(void**)&p_ion_open = dlsym(h, "ion_open");
    *(void**)&p_ion_alloc_fd = dlsym(h, "ion_alloc_fd");
    *(void**)&p_ion_heap_cnt = dlsym(h, "ion_query_heap_cnt");
    *(void**)&p_ion_get_heaps = dlsym(h, "ion_query_get_heaps");
    *(void**)&p_ion_import = dlsym(h, "ion_import");
    *(void**)&p_ion_map = dlsym(h, "ion_map");
    *(void**)&p_ion_free = dlsym(h, "ion_free");
    LOGI("ion: open=%p alloc_fd=%p import=%p map=%p free=%p heaps=%p/%p",
         (void*)p_ion_open, (void*)p_ion_alloc_fd, (void*)p_ion_import,
         (void*)p_ion_map, (void*)p_ion_free, (void*)p_ion_heap_cnt, (void*)p_ion_get_heaps);
}

static bool ob_alloc_one(size_t len, int* out_fd) {
    load_ion();
    static unsigned sys_mask;
    if (!p_ion_open || !p_ion_alloc_fd) return false;
    if (g_ion_fd < 0) g_ion_fd = p_ion_open();
    if (g_ion_fd < 0) return false;

    if (!sys_mask && p_ion_heap_cnt && p_ion_get_heaps) {
        int cnt = 0;
        if (p_ion_heap_cnt(g_ion_fd, &cnt) == 0 && cnt > 0 && cnt <= 64) {
            struct ion_heap_data heaps[64];
            memset(heaps, 0, sizeof(heaps));
            if (p_ion_get_heaps(g_ion_fd, cnt, heaps) == 0) {
                for (int i = 0; i < cnt; i++)
                    LOGI("ownbuf: heap %d '%s' type=%u id=%u", i, heaps[i].name,
                         heaps[i].type, heaps[i].heap_id);
                for (int i = 0; i < cnt; i++)
                    if (strcmp(heaps[i].name, "ion_system_heap") == 0)
                        sys_mask = 1u << heaps[i].heap_id;
            }
        }
    }

    unsigned mask = g_ob_heap >= 0 ? (unsigned)g_ob_heap
                                   : (sys_mask ? sys_mask : 0xffffffffu);
    unsigned flags = g_ob_flags >= 0 ? (unsigned)g_ob_flags
                                     : (ION_FLAG_CACHED | ION_FLAG_CACHED_NEEDS_SYNC);
    int fd = -1;
    int rc = p_ion_alloc_fd(g_ion_fd, len, 4096, mask, flags, &fd);
    if ((rc != 0 || fd < 0) && g_ob_flags < 0) {
        int e1 = errno;
        rc = p_ion_alloc_fd(g_ion_fd, len, 4096, mask, 0, &fd);
        if (rc != 0 || fd < 0) LOGE("ownbuf: ion_alloc_fd(%zu) mask=0x%x: %d/%s (cached: %s)",
                                    len, mask, rc, strerror(errno), strerror(e1));
    } else if (rc != 0 || fd < 0) {
        LOGE("ownbuf: ion_alloc_fd(%zu) mask=0x%x flags=0x%x: %d/%s",
             len, mask, flags, rc, strerror(errno));
    }
    if (rc != 0 || fd < 0) return false;
    *out_fd = fd;
    LOGI("ownbuf: allocated %zu bytes mask=0x%x flags=0x%x fd=%d", len, mask, flags, fd);
    return true;
}

static bool ob_alloc(int w, int h) {
    if (g_ob_count > 0) return g_ob[0].w == w && g_ob[0].h == h;
    size_t len = (size_t)w * (size_t)h;
    if (len == 0 || len > (64u << 20)) return false;
    for (int i = 0; i < 3; i++) {
        int fd = -1;
        if (!ob_alloc_one(len, &fd)) return false;
        void* p = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            LOGE("ownbuf: mmap(%d, %zu): %s", fd, len, strerror(errno));
            close(fd);
            return false;
        }
        g_ob[i] = OwnBuf{ fd, (uint8_t*)p, len, w, h };
    }
    g_ob_count = 3;
    LOGI("ownbuf: %d x %dx%d gray buffers allocated (%zu bytes each)", g_ob_count, w, h, len);
    return true;
}

static void ob_sync(int fd, bool start) {
#if HAVE_DMABUF_SYNC
    struct dma_buf_sync sy = {};
    sy.flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | DMA_BUF_SYNC_WRITE;
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sy);
#else
    (void)fd; (void)start;
#endif
}

static int g_fast_active;             // atomic: drop the vendor's commits (we own the panel)
static int g_power_on = 1;
static pthread_mutex_t g_commit_lock = PTHREAD_MUTEX_INITIALIZER;   // one ioctl at a time
static uint64_t g_last_fast_ns, g_last_vend_ns;
static uint32_t g_last_vendor_dither = 0xffffffff;
static uint64_t g_fast_commits, g_fast_skips, g_fast_fails;
static uint64_t g_fast_us_sum, g_fast_us_max;
static uint64_t g_fast_dt_sum, g_fast_dt_max, g_last_fast_commit_ns;

// mapped client target + shadow copy for the pixel diff. The client target fd
// rotates between 2-3 buffers and mmap/munmap of 4 MB costs tens of ms on this
// SoC, so keep a small ring of mappings instead of remapping every frame.
struct CtMap {
    int fd;
    uint8_t* map;
    size_t len;
    uint64_t dev;
    uint64_t ino;
    int ion_handle;  // owned by libion; must be freed before replacing/unmapping
    int map_fd;      // libion returns an independently allocated dma-buf fd
    int users;       // active ct_map leases; eviction skips these slots
};
static CtMap g_ctmaps[4] = {
    { -1, nullptr, 0, 0, 0, -1, -1, 0 },
    { -1, nullptr, 0, 0, 0, -1, -1, 0 },
    { -1, nullptr, 0, 0, 0, -1, -1, 0 },
    { -1, nullptr, 0, 0, 0, -1, -1, 0 },
};
static int g_ctmap_next;
static pthread_mutex_t g_ctmap_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t* g_shadow;
static size_t   g_shadow_len;


// ---------------------------------------------------------------- state

static void* g_real_handle;
static hw_module_t* g_real_module;
static hwc2_device_t* g_dev;
static hwc2_function_pointer_t (*g_real_getFunction)(hwc2_device_t*, int32_t);
static hwc2_function_pointer_t g_real_fn[FN_MAX_PROBE];

extern "C" __attribute__((visibility("default"))) hw_module_t HMI;   // defined at the end

static int g_log = 1;
static int g_rect = 0;         // 0 observe, 1 rewrite
static int g_mode = -1;        // forced e-ink mode, -1 = vendor
static int g_global_mode = -1; // vendor.xrz.global_refresh_mode (E-ink Center)
static int g_src = 1;          // 1 client target, 2 layer union
static int g_xform = 0;        // see header
static int g_min_pct = 1;
static int g_log_every = 60;
static int g_test_w = 0;       // test_w/test_h: force a fixed top-left rect (timing probe)
static int g_test_h = 0;
static int g_flag = -1;        // force cfg->flag (default leaves the vendor's 1)
static int g_nofence = 0;      // 1 = do not hand SF a present fence (test: is SF the throttle?)
static int g_test_height = 0;  // force cfg->height (test: does the controller scan `height` lines?)
static int g_probe = 0;        // 1 = also time convertLayerToGray (processImage is always hooked)
static bool g_buildid_ok;      // real library is the BuildID the struct offsets were recovered from
static int g_novsync = 0;      // 1 = mtk_waitVSync returns immediately (test: is the composer pacing on it?)
static uint64_t g_vsync_calls;
static uint64_t g_vsync_us_total;

static int g_display_w = 720;  // SurfaceFlinger display (quadrant/physical, not e-ink buffer)
static int g_display_h = 1440;
static int g_panel_w = 1440;   // e-ink controller buffer (learned from commits)
static int g_panel_h = 720;

struct Rect { int l, t, r, b; };   // inclusive; empty when r < l or b < t
static inline bool rect_empty(const Rect& q) { return q.r < q.l || q.b < q.t; }

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Rect g_pend_ct = {0, 0, -1, -1};      // client-target damage since last commit
static Rect g_pend_layer = {0, 0, -1, -1};   // layer damage since last commit
static int  g_pend_ct_rects, g_pend_layer_rects;
static Rect g_last_ct = {0, 0, -1, -1};      // last frame's CT damage (for logging)
static Rect g_last_layer = {0, 0, -1, -1};
static uint64_t g_present, g_commits, g_rewrites, g_logged;
static uint64_t g_last_commit_ns;
static uint64_t g_last_present_ns;   // end of the last presentDisplay
static uint32_t g_vendor_mode = 0xffffffff;
static Rect g_vendor_rect = {0, 0, -1, -1};
static Rect g_new_rect = {0, 0, -1, -1};
static uint32_t g_new_mode = 0xffffffff;
static int g_last_ret;
static int g_nlayers, g_nclient, g_ndevice;

static uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int prop_int(const char* name, int def) {
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get(name, buf) <= 0) return def;
    return atoi(buf);
}

static void read_knobs() {
    g_log = prop_int("vendor.debug.hwc.shim.log", 1);
    g_rect = prop_int("vendor.debug.hwc.shim.rect", 0);
    g_mode = prop_int("vendor.debug.hwc.shim.mode", -1);
    g_global_mode = prop_int("vendor.xrz.global_refresh_mode", -1);
    g_src = prop_int("vendor.debug.hwc.shim.src", 1);
    g_xform = prop_int("vendor.debug.hwc.shim.xform", 0);
    g_min_pct = prop_int("vendor.debug.hwc.shim.min_pct", 1);
    g_log_every = prop_int("vendor.debug.hwc.shim.log_every", 60);
    g_test_w = prop_int("vendor.debug.hwc.shim.test_w", 0);
    g_test_h = prop_int("vendor.debug.hwc.shim.test_h", 0);
    g_flag = prop_int("vendor.debug.hwc.shim.flag", -1);
    g_nofence = prop_int("vendor.debug.hwc.shim.nofence", 0);
    g_test_height = prop_int("vendor.debug.hwc.shim.test_height", 0);
    g_novsync = prop_int("vendor.debug.hwc.shim.novsync", 0);
    g_probe = prop_int("vendor.debug.hwc.shim.probe", 0);
    g_use_ct = prop_int("vendor.debug.hwc.shim.use_ct", 0);
    g_ct_format = prop_int("vendor.debug.hwc.shim.ct_format", -1);
    g_fast = prop_int("vendor.debug.hwc.shim.fast", 0);
    g_fast_idle = prop_int("vendor.debug.hwc.shim.fast_idle", 1);
    g_fast_hz = prop_int("vendor.debug.hwc.shim.fast_hz", 0);
    g_fast_diff = prop_int("vendor.debug.hwc.shim.fast_diff", 0);
    g_fast_wait_ms = prop_int("vendor.debug.hwc.shim.fast_wait_ms", 40);
    g_inplace = prop_int("vendor.debug.hwc.shim.inplace", 0);
    g_inplace_dither = prop_int("vendor.debug.hwc.shim.inplace_dither", 1);
    g_vend_hz = prop_int("vendor.debug.hwc.shim.vend_hz", 0);
    g_dither = prop_int("vendor.debug.hwc.shim.dither", -1);
    g_skipconv = prop_int("vendor.debug.hwc.shim.skipconv", 0);
    g_skipconv_fmt = prop_int("vendor.debug.hwc.shim.skipconv_fmt", 1);
    g_fastgray = prop_int("vendor.debug.hwc.shim.fastgray", 0);
    g_fastconv = prop_int("vendor.debug.hwc.shim.fastconv", 0);
    g_dither1 = prop_int("vendor.debug.hwc.shim.dither1", 1);
    g_verify = prop_int("vendor.debug.hwc.shim.verify", 0);
    g_conv_dst_pitch = prop_int("vendor.debug.hwc.shim.conv_dst_pitch", -1);
    g_skipcopy = prop_int("vendor.debug.hwc.shim.skipcopy", 0);
    g_damagecopy = prop_int("vendor.debug.hwc.shim.damagecopy", 0);
    g_xproc = prop_int("vendor.debug.hwc.shim.xproc", 0);
    g_xproc_vendor = prop_int("vendor.debug.hwc.shim.xproc_vendor", 0);
    g_xproc_damage = prop_int("vendor.debug.hwc.shim.xproc_damage", 1);
    g_helper_onebit = prop_int("vendor.debug.hwc.shim.helper_onebit", 0);
    g_ctsrc = prop_int("vendor.debug.hwc.shim.ctsrc", 0);
    g_ownbuf = prop_int("vendor.debug.hwc.shim.ownbuf", 0);
    g_ob_heap = prop_int("vendor.debug.hwc.shim.ob_heap", -1);
    g_ob_flags = prop_int("vendor.debug.hwc.shim.ob_flags", -1);
    g_ctprobe = prop_int("vendor.debug.hwc.shim.ctprobe", 0);
    g_vendor_fd_probe = prop_int("vendor.debug.hwc.shim.vendor_fd_probe", 0);
    if (g_log_every < 1) g_log_every = 1;
}

// Knobs are re-read at most once a second so log level / rect / mode can be
// changed on a running composer instead of paying a restart per experiment.
static void maybe_reload_knobs() {
    static uint64_t next_ns;
    uint64_t t = now_ns();
    if (t < next_ns) return;
    next_ns = t + 1000000000ull;
    read_knobs();
}

static inline void union_rect(Rect& acc, int l, int t, int r, int b) {
    if (r < l || b < t) return;
    if (rect_empty(acc)) { acc.l = l; acc.t = t; acc.r = r; acc.b = b; return; }
    if (l < acc.l) acc.l = l;
    if (t < acc.t) acc.t = t;
    if (r > acc.r) acc.r = r;
    if (b > acc.b) acc.b = b;
}

static inline long area(const Rect& q) {
    if (rect_empty(q)) return 0;
    return (long)(q.r - q.l + 1) * (long)(q.b - q.t + 1);
}

// SurfaceFlinger display space -> e-ink controller buffer space.
// Which transform is right is not knowable offline; log first, then set xform.
static Rect xform_rect(const Rect& in) {
    if (rect_empty(in)) return in;
    Rect q = in;
    const int W = g_panel_w - 1, H = g_panel_h - 1;
    switch (g_xform) {
        case 0: break;                                             // identity
        case 1: q.l = in.t; q.t = in.l; q.r = in.b; q.b = in.r; break;   // swap x/y
        case 2: { Rect s = {in.t, in.l, in.b, in.r};                // swap + mirror x
                  q.l = W - s.r; q.r = W - s.l; q.t = s.t; q.b = s.b; } break;
        case 3: { Rect s = {in.t, in.l, in.b, in.r};                // swap + mirror y
                  q.t = H - s.b; q.b = H - s.t; q.l = s.l; q.r = s.r; } break;
        case 4: q.l = W - in.r; q.r = W - in.l;                     // rot180
                q.t = H - in.b; q.b = H - in.t; break;
        case 5: { Rect s = {W - in.r, H - in.b, W - in.l, H - in.t};   // rot180 + swap
                  q.l = s.t; q.t = s.l; q.r = s.b; q.b = s.r; } break;
        default: break;
    }
    if (q.l < 0) q.l = 0;
    if (q.t < 0) q.t = 0;
    if (q.r > W) q.r = W;
    if (q.b > H) q.b = H;
    return q;
}

// ---------------------------------------------------------------- fast path

static void ct_snap_clear(CtSnap* s) {
    s->fd = -1;
    s->fence = -1;
    s->w = s->h = s->stride = s->vstride = s->format = 0;
    s->usage = 0;
    s->valid = false;
    s->handle = nullptr;
}

static void ct_snap_release(CtSnap* s) {
    if (!s) return;
    if (s->fd >= 0) close(s->fd);
    if (s->fence >= 0) close(s->fence);
    if (s->handle) native_handle_delete(s->handle);
    ct_snap_clear(s);
}

struct SnapLease {
    CtSnap snap;
    SnapLease() { ct_snap_clear(&snap); }
    ~SnapLease() { ct_snap_release(&snap); }
};

// Caller holds g_lock. The duplicate owns its fd, fence and native handle.
static bool ct_snap_clone_locked(const CtSnap& src, CtSnap* out) {
    ct_snap_clear(out);
    if (!src.valid || src.fd < 0) return false;
    int fd = dup(src.fd);
    if (fd < 0) return false;
    int fence = src.fence >= 0 ? dup(src.fence) : -1;
    if (src.fence >= 0 && fence < 0) {
        close(fd);
        return false;
    }
    native_handle_t* handle = src.handle ? native_handle_clone(src.handle) : nullptr;
    if (src.handle && !handle) {
        close(fd);
        if (fence >= 0) close(fence);
        return false;
    }
    *out = CtSnap{ fd, fence, src.w, src.h, src.stride, src.vstride, src.format,
                   src.usage, true, handle };
    return true;
}

static bool fd_identity(int fd, uint64_t* dev, uint64_t* ino) {
    if (fd < 0 || !dev || !ino) return false;
    struct stat st = {};
    if (fstat(fd, &st) != 0) return false;
    // dma-buf fds are anonymous inodes. A zero inode cannot identify a buffer.
    if (st.st_ino == 0) return false;
    *dev = (uint64_t)st.st_dev;
    *ino = (uint64_t)st.st_ino;
    return true;
}

// Caller holds g_lock. Keep one entry per dma-buf identity; SurfaceFlinger
// rotates 2-3 client targets, so this is a small, bounded history.
static void snap_hist_push_locked(const CtSnap& s, uint64_t seq, uint64_t dev,
                                  uint64_t ino) {
    if (!s.valid || ino == 0) return;
    int slot = -1;
    for (int i = 0; i < 8; ++i) {
        if (g_snap_hist[i].valid && g_snap_hist[i].dev == dev &&
            g_snap_hist[i].ino == ino) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < 8; ++i) {
            if (!g_snap_hist[i].valid) { slot = i; break; }
        }
    }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < 8; ++i) {
            if (g_snap_hist[i].seq < g_snap_hist[slot].seq) slot = i;
        }
    }
    CtSnap clone;
    if (!ct_snap_clone_locked(s, &clone)) return;
    if (g_snap_hist[slot].valid) ct_snap_release(&g_snap_hist[slot].snap);
    g_snap_hist[slot] = SnapHist{ clone, seq, dev, ino, true };
}

// Caller holds g_lock.
static bool snap_hist_clone_by_fd_locked(int fd, CtSnap* out, uint64_t* seq) {
    uint64_t dev = 0, ino = 0;
    if (!fd_identity(fd, &dev, &ino)) return false;
    int best = -1;
    for (int i = 0; i < 8; ++i) {
        if (!g_snap_hist[i].valid || g_snap_hist[i].dev != dev ||
            g_snap_hist[i].ino != ino) continue;
        if (best < 0 || g_snap_hist[i].seq > g_snap_hist[best].seq) best = i;
    }
    if (best < 0) return false;
    if (!ct_snap_clone_locked(g_snap_hist[best].snap, out)) return false;
    if (seq) *seq = g_snap_hist[best].seq;
    return true;
}

// Clone the client target and its acquire fence before the vendor HWC consumes
// the fence. The candidate is not visible to the worker until setClientTarget
// returns success.
static bool snap_build(buffer_handle_t h, int fence, CtSnap* out,
                       uint64_t* snap_dev, uint64_t* snap_ino) {
    ct_snap_clear(out);
    *snap_dev = 0;
    *snap_ino = 0;
    int fd = -1, fdup = -1, w = 0, hh = 0, stride = 0, vstride = 0, fmt = 0;
    uint64_t usage = 0;
    if (h) {
        if (real_gralloc_extra_query) {
            int32_t v = 0;
            if (real_gralloc_extra_query(h, 1, &v) == 0) fd = v;
            v = 0; if (real_gralloc_extra_query(h, 10, &v) == 0) w = v;
            v = 0; if (real_gralloc_extra_query(h, 11, &v) == 0) hh = v;
            v = 0; if (real_gralloc_extra_query(h, 12, &v) == 0) stride = v;
            v = 0; if (real_gralloc_extra_query(h, 13, &v) == 0) vstride = v;
            v = 0; if (real_gralloc_extra_query(h, 15, &v) == 0) fmt = v;
            int32_t uv = 0; if (real_gralloc_extra_query(h, 16, &uv) == 0) usage = (uint32_t)uv;
        }
        if (fd < 0 && h->numFds > 0) fd = h->data[0];
    }
    if (fd >= 0) fd = dup(fd);
    if (fd >= 0) fd_identity(fd, snap_dev, snap_ino);
    if (fd >= 0) fdup = (fence >= 0) ? dup(fence) : -1;
    native_handle_t* cloned = h ? native_handle_clone(h) : nullptr;
    if (fd < 0 || w <= 0 || hh <= 0 || (fence >= 0 && fdup < 0) ||
        (h && !cloned)) {
        if (fd >= 0) close(fd);
        if (fdup >= 0) close(fdup);
        if (cloned) native_handle_delete(cloned);
        return false;
    }
    if (stride <= 0) stride = w;
    if (vstride <= 0) vstride = hh;
    *out = CtSnap{ fd, fdup, w, hh, stride, vstride, fmt, usage, true, cloned };
    return true;
}

// Publish a candidate only after the real setClientTarget accepted it. The
// candidate's ownership transfers to g_snap (or is released on failure).
static void snap_publish(CtSnap* candidate, bool valid, uint64_t dev, uint64_t ino) {
    CtSnap old;
    pthread_mutex_lock(&g_lock);
    old = g_snap;
    if (valid) {
        g_snap = *candidate;
        ct_snap_clear(candidate);
        g_snap_seq++;
        if (g_xproc || g_xproc_vendor)
            snap_hist_push_locked(g_snap, (uint64_t)g_snap_seq, dev, ino);
    } else {
        g_snap = CtSnap{ -1, -1, 0, 0, 0, 0, 0, 0, false };
        for (int i = 0; i < 8; ++i) {
            if (!g_snap_hist[i].valid) continue;
            ct_snap_release(&g_snap_hist[i].snap);
            g_snap_hist[i].valid = false;
        }
    }
    pthread_mutex_unlock(&g_lock);
    ct_snap_release(&old);
    if (!valid) ct_snap_release(candidate);
}

// A sync fence becomes readable when signalled, so poll() is the whole wait.
static bool wait_fence(int fd, int timeout_ms) {
    if (fd < 0) { g_last_wait_us = 0; return true; }
    struct pollfd p = { fd, POLLIN, 0 };
    uint64_t t0 = now_ns();
    int r = poll(&p, 1, timeout_ms);
    g_last_wait_us = (now_ns() - t0) / 1000;
    return r > 0 && (p.revents & POLLIN) != 0;
}

static void ct_map_evict(int i) {
    if (i < 0 || i >= 4) return;
    CtMap& m = g_ctmaps[i];
    if (m.users != 0) return;  // caller selects an unreferenced slot
    if (m.map) { munmap(m.map, m.len); m.map = nullptr; }
    if (m.ion_handle >= 0 && g_ion_fd >= 0 && p_ion_free) {
        p_ion_free(g_ion_fd, m.ion_handle);
    }
    if (m.map_fd >= 0) { close(m.map_fd); }
    m.fd = -1;
    m.dev = m.ino = 0;
    m.len = 0;
    m.ion_handle = m.map_fd = -1;
}

static bool ct_map_locked(const CtSnap& s, bool write, uint8_t** base, size_t* len);

static bool ct_map(const CtSnap& s, bool write, uint8_t** base, size_t* len) {
    pthread_mutex_lock(&g_ctmap_lock);
    const bool result = ct_map_locked(s, write, base, len);
    pthread_mutex_unlock(&g_ctmap_lock);
    return result;
}

static bool ct_map_locked(const CtSnap& s, bool write, uint8_t** base, size_t* len) {
    load_ion();
    size_t need = (size_t)s.stride * 4 * (size_t)(s.vstride > s.h ? s.vstride : s.h);
    if (need == 0) return false;
    uint64_t need_dev = 0, need_ino = 0;
    const bool have_identity = fd_identity(s.fd, &need_dev, &need_ino);
    for (int i = 0; i < 4; i++) {
        const bool same = have_identity
                              ? (g_ctmaps[i].dev == need_dev && g_ctmaps[i].ino == need_ino)
                              : (g_ctmaps[i].fd == s.fd);
        if (g_ctmaps[i].map && same && g_ctmaps[i].len >= need) {
            g_ctmaps[i].users++;
            *base = g_ctmaps[i].map; *len = g_ctmaps[i].len;
#if HAVE_DMABUF_SYNC
            struct dma_buf_sync sy = {};
            sy.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ | (write ? DMA_BUF_SYNC_WRITE : 0);
            ioctl(s.fd, DMA_BUF_IOCTL_SYNC, &sy);
#endif
            return true;
        }
    }
    int i = -1;
    for (int n = 0; n < 4; ++n) {
        const int cand = (g_ctmap_next + n) & 3;
        if (g_ctmaps[cand].users == 0) { i = cand; break; }
    }
    if (i < 0) {
        LOGE("client target map cache busy (4 active leases)");
        return false;
    }
    g_ctmap_next = (i + 1) & 3;
    ct_map_evict(i);
    int prot = PROT_READ | (write ? PROT_WRITE : 0);
    void* p = MAP_FAILED;
    int ion_handle = -1, map_fd = -1;
    // The vendor's converter maps its buffers through ION (import + map), which
    // gives a cached CPU mapping. A plain mmap of the same dma-buf is uncached
    // on this kernel (~13 MB/s, 80 ms for a 1440x720 frame), so prefer ION.
    if (p_ion_import && p_ion_map) {
        if (g_ion_fd < 0 && p_ion_open) g_ion_fd = p_ion_open();
        if (g_ion_fd >= 0) {
            int handle = 0;
            if (p_ion_import(s.fd, &handle) == 0) {
                ion_handle = handle;
                unsigned char* ptr = nullptr;
                if (p_ion_map(g_ion_fd, handle, need, prot, MAP_SHARED, 0, &ptr, &map_fd) == 0 &&
                    ptr) {
                    p = (void*)ptr;
                    if (g_log >= 1 && g_ctmap_next <= 4)
                        LOGI("ct_map: ion import/map ok (fd=%d %zu bytes, map_fd=%d)",
                             s.fd, need, map_fd);
                } else {
                    if (g_log >= 1 && g_ctmap_next <= 4)
                        LOGE("ct_map: ion_map failed: %s", strerror(errno));
                }
            } else if (g_log >= 1 && g_ctmap_next <= 4) {
                LOGE("ct_map: ion_import(%d) failed: %s", s.fd, strerror(errno));
            }
        }
    }
    if (p == MAP_FAILED) {
        // The import owns a kernel reference; release it before using raw mmap.
        if (ion_handle >= 0 && g_ion_fd >= 0 && p_ion_free) {
            p_ion_free(g_ion_fd, ion_handle);
            ion_handle = -1;
        }
        if (map_fd >= 0) { close(map_fd); map_fd = -1; }
        p = mmap(nullptr, need, prot, MAP_SHARED, s.fd, 0);
    }
    if (p == MAP_FAILED) {
        LOGE("client target mmap(fd=%d, %zu) failed: %s", s.fd, need, strerror(errno));
        if (ion_handle >= 0 && g_ion_fd >= 0 && p_ion_free) p_ion_free(g_ion_fd, ion_handle);
        if (map_fd >= 0) close(map_fd);
        return false;
    }
    g_ctmaps[i] = CtMap{ s.fd, (uint8_t*)p, need, need_dev, need_ino,
                         ion_handle, map_fd, 1 };
#if HAVE_DMABUF_SYNC
    struct dma_buf_sync sy = {};
    sy.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ | (write ? DMA_BUF_SYNC_WRITE : 0);
    ioctl(s.fd, DMA_BUF_IOCTL_SYNC, &sy);
#endif
    *base = g_ctmaps[i].map; *len = g_ctmaps[i].len;
    return true;
}

static void ct_unmap_sync(const CtSnap& s, bool wrote) {
#if HAVE_DMABUF_SYNC
    struct dma_buf_sync sy = {};
    sy.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ | (wrote ? DMA_BUF_SYNC_WRITE : 0);
    ioctl(s.fd, DMA_BUF_IOCTL_SYNC, &sy);
#endif
    (void)s; (void)wrote;
    pthread_mutex_lock(&g_ctmap_lock);
    uint64_t dev = 0, ino = 0;
    if (fd_identity(s.fd, &dev, &ino)) {
        for (int i = 0; i < 4; ++i) {
            if (g_ctmaps[i].map && g_ctmaps[i].dev == dev && g_ctmaps[i].ino == ino &&
                g_ctmaps[i].users > 0) {
                g_ctmaps[i].users--;
                break;
            }
        }
    } else {
        for (int i = 0; i < 4; ++i) {
            if (g_ctmaps[i].map && g_ctmaps[i].fd == s.fd && g_ctmaps[i].users > 0) {
                g_ctmaps[i].users--;
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_ctmap_lock);
}

// Compare the client target with the previous committed frame and return the
// changed bounding box. This is what turns continuous presenting into small
// panel updates: a clock tick, a cursor blink or a typing line is a handful of
// rows instead of 1440. Rows are updated in the shadow as they are found dirty.
static bool diff_bbox(const CtSnap& s, const uint8_t* cur, int pitch, int h, Rect* out) {
    size_t len = (size_t)pitch * (size_t)h;
    if (!g_shadow || g_shadow_len != len) {
        free(g_shadow);
        g_shadow = (uint8_t*)malloc(len);
        g_shadow_len = g_shadow ? len : 0;
        if (!g_shadow) return false;
        memcpy(g_shadow, cur, len);
        *out = Rect{ 0, 0, s.w - 1, h - 1 };
        return true;
    }
    int l = s.w, t = h, r = -1, b = -1;
    for (int y = 0; y < h; y++) {
        const uint8_t* a = cur + (size_t)y * pitch;
        uint8_t* c = g_shadow + (size_t)y * pitch;
        if (memcmp(a, c, pitch) == 0) continue;
        int fl = 0, fr = pitch - 1;
        while (fl < pitch && a[fl] == c[fl]) fl++;
        while (fr > fl && a[fr] == c[fr]) fr--;
        memcpy(c, a, pitch);
        int x0 = fl / 4, x1 = fr / 4;
        if (x0 < l) l = x0;
        if (x1 > r) r = x1;
        if (y < t) t = y;
        b = y;
    }
    if (r < 0) return false;
    if (l < 0) l = 0;
    if (r > s.w - 1) r = s.w - 1;
    *out = Rect{ l, t, r, b };
    return true;
}

// RGBA8888 -> gray8, `w` pixels x `h` rows, src pitch `src_pitch` bytes, dst
// pitch `dst_pitch` bytes. Ordered 4x4 dither to the panel's 16 levels, no
// division anywhere. NEON on arm64 (16 pixels per iteration), scalar tail.
static const uint8_t kBayer16[4][16] = {
    {  0,  8,  2, 10,  0,  8,  2, 10,  0,  8,  2, 10,  0,  8,  2, 10 },
    { 12,  4, 14,  6, 12,  4, 14,  6, 12,  4, 14,  6, 12,  4, 14,  6 },
    {  3, 11,  1,  9,  3, 11,  1,  9,  3, 11,  1,  9,  3, 11,  1,  9 },
    { 15,  7, 13,  5, 15,  7, 13,  5, 15,  7, 13,  5, 15,  7, 13,  5 },
};

// 8x8 Bayer for the 1-bit modes (A2/Hyper/Extreme). The kernel does not dither
// those, so a mid-grey would become a partial drive and leave residue; a dot
// pattern drives every pixel fully and stays stable.
static const uint8_t kBayer8[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 },
    { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 },
    { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 },
    { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 },
    { 63, 31, 55, 23, 61, 29, 53, 21 },
};

static inline uint8_t gray_of3(unsigned r, unsigned g, unsigned b, unsigned dith) {
    unsigned l = (r * 77u + g * 150u + b * 29u) >> 8;      // 0..255
    unsigned q = (l + dith) >> 4;                          // 0..16
    if (q > 15) q = 15;
    return (uint8_t)(q * 17u);
}

static void gray_conv(uint8_t* dst, const uint8_t* src, int w, int h,
                      int src_pitch, int dst_pitch, bool onebit) {
    if (w <= 0 || h <= 0) return;
    if (onebit) {
        for (int y = 0; y < h; y++) {
            const uint8_t* s = src + (size_t)y * src_pitch;
            uint8_t* d = dst + (size_t)y * dst_pitch;
            for (int x = 0; x < w; x++) {
                const uint8_t* px = s + (size_t)x * 4;
                unsigned l = (px[0] * 77u + px[1] * 150u + px[2] * 29u) >> 8;
                unsigned t = (unsigned)kBayer8[y & 7][x & 7] * 4u + 2u;   // 2..254
                d[x] = (uint8_t)(l > t ? 255 : 0);
            }
        }
        return;
    }
#if defined(__aarch64__)
    for (int y = 0; y < h; y++) {
        const uint8_t* s = src + (size_t)y * src_pitch;
        uint8_t* d = dst + (size_t)y * dst_pitch;
        uint8x16_t bv = g_inplace_dither ? vld1q_u8(kBayer16[y & 3]) : vdupq_n_u8(0);
        uint16x8_t bl = vmovl_u8(vget_low_u8(bv));
        uint16x8_t bh = vmovl_u8(vget_high_u8(bv));
        int x = 0;
        for (; x + 16 <= w; x += 16) {
            uint8x16x4_t p = vld4q_u8(s + (size_t)x * 4);
            uint16x8_t l0 = vmulq_n_u16(vmovl_u8(vget_low_u8(p.val[0])), 77);
            l0 = vmlaq_n_u16(l0, vmovl_u8(vget_low_u8(p.val[1])), 150);
            l0 = vmlaq_n_u16(l0, vmovl_u8(vget_low_u8(p.val[2])), 29);
            uint16x8_t l1 = vmulq_n_u16(vmovl_u8(vget_high_u8(p.val[0])), 77);
            l1 = vmlaq_n_u16(l1, vmovl_u8(vget_high_u8(p.val[1])), 150);
            l1 = vmlaq_n_u16(l1, vmovl_u8(vget_high_u8(p.val[2])), 29);
            uint16x8_t q0 = vshrq_n_u16(vaddq_u16(vshrq_n_u16(l0, 8), bl), 4);
            uint16x8_t q1 = vshrq_n_u16(vaddq_u16(vshrq_n_u16(l1, 8), bh), 4);
            q0 = vminq_u16(q0, vdupq_n_u16(15));
            q1 = vminq_u16(q1, vdupq_n_u16(15));
            uint8x16_t out = vcombine_u8(vmovn_u16(vmulq_n_u16(q0, 17)),
                                         vmovn_u16(vmulq_n_u16(q1, 17)));
            vst1q_u8(d + x, out);
        }
        for (; x < w; x++) {
            const uint8_t* px = s + (size_t)x * 4;
            d[x] = gray_of3(px[0], px[1], px[2], g_inplace_dither ? kBayer16[y & 3][x & 15] : 0);
        }
    }
#else
    for (int y = 0; y < h; y++) {
        const uint8_t* s = src + (size_t)y * src_pitch;
        uint8_t* d = dst + (size_t)y * dst_pitch;
        for (int x = 0; x < w; x++) {
            const uint8_t* px = s + (size_t)x * 4;
            d[x] = gray_of3(px[0], px[1], px[2], g_inplace_dither ? kBayer16[y & 3][x & 15] : 0);
        }
    }
#endif
}

// RGBA -> gray8 in place, rows [t,b], source pitch `src_pitch`, destination
// pitch `w`. Forward-safe: byte i is written only after pixel i (bytes
// 4i..4i+3) has been read, and the destination pitch is never larger than the
// source pitch, so a row never overwrites unread source.
static void gray_rows(uint8_t* base, int src_pitch, int w, int t, int b) {
    if (t < 0) t = 0;
    if (b < t) return;
    gray_conv(base + (size_t)t * w, base + (size_t)t * src_pitch, w, b - t + 1, src_pitch, w, false);
}

// RGBA -> gray8 in place on the client target, rows [t,b].
static void inplace_gray(CtSnap& s, int t, int b) {
    if (t < 0) t = 0;
    if (b > s.h - 1) b = s.h - 1;
    uint8_t* base = nullptr; size_t len = 0;
    if (!ct_map(s, true, &base, &len)) return;
    gray_rows(base, s.stride * 4, s.w, t, b);
    ct_unmap_sync(s, true);
}

// ---------------------------------------------------------------- mapper probe
//
// The client target is a gralloc buffer. Two ways to read it from here:
//
//   raw mmap of its dma-buf fd  -- what ct_map() falls back to today; measured
//                                  13-124 MB/s (31-80 ms per 4 MB), i.e. the
//                                  ceiling on this GSI;
//   the gralloc HAL ('mapper')  -- GraphicBufferMapper::lock(), which is the
//                                  same call the vendor composer itself imports
//                                  (it is PLT-imported by hwcomposer.mt6765.so,
//                                  so libui is already loaded in this process).
//
// A standalone AHardwareBuffer experiment (gpu-gray/) reads a normally allocated
// ---------------------------------------------------------------- commit path
// (restored: removed by a concurrent edit while the mapper probe was added)

static inline void clamp_panel(Rect& q) {
    if (q.l < 0) q.l = 0;
    if (q.t < 0) q.t = 0;
    if (q.r > g_panel_w - 1) q.r = g_panel_w - 1;
    if (q.b > g_panel_h - 1) q.b = g_panel_h - 1;
}

// Convert through a separate process. gralloc.rogue can import and cache-lock a
// foreign SurfaceFlinger handle in a normal helper process (~2.2 ms), but faults
// when the same import is done inside the vendor composer. The helper returns an
// RGBA-backed dma-buf whose first width*height bytes are packed gray8.
static void xproc_disconnect_locked() {
    if (g_xproc_sock >= 0) close(g_xproc_sock);
    g_xproc_sock = -1;
}

static bool xproc_accept_locked() {
    // HWC owns an abstract listener, so there is no filesystem path for the
    // composer to search and no socket-file label to relax. The root helper
    // connects to us and stays in its own process/domain. Keep the connection
    // persistent across frames unless an error occurs.
    if (g_xproc_sock >= 0) {
        pollfd sp{g_xproc_sock, POLLIN, 0};
        const int polled = poll(&sp, 1, 0);
        if (polled == 0) {
            if (g_xproc_attempts >= 3) {
                timeval timeout{0, 350 * 1000};
                setsockopt(g_xproc_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                setsockopt(g_xproc_sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            }
            return true;
        }
        // Any readable/error/hup state is unexpected between complete
        // SOCK_SEQPACKET exchanges; discard it and accept a fresh helper.
        xproc_disconnect_locked();
    }
    if (g_xproc_listen < 0) {
        int listener = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (listener < 0) return false;
        constexpr const char* name = "hwcgray_hwc";
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        addr.sun_path[0] = '\0';
        snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1, "%s", name);
        const socklen_t len = (socklen_t)(offsetof(sockaddr_un, sun_path) + 1 + strlen(name));
        if (bind(listener, reinterpret_cast<sockaddr*>(&addr), len) != 0 ||
            listen(listener, 4) != 0) {
            close(listener);
            return false;
        }
        g_xproc_listen = listener;
    }

    pollfd p{g_xproc_listen, POLLIN, 0};
    if (poll(&p, 1, 50) <= 0) return false;
    int fd = accept(g_xproc_listen, nullptr, nullptr);
    if (fd < 0) return false;
    const int timeout_ms = g_xproc_attempts < 3 ? 3000 : 350;
    timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        close(fd);
        return false;
    }
    g_xproc_sock = fd;
    return true;
}

static void vendor_capture_commit_fd(int fd) {
    if (fd < 0) return;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_vendor_commit_fd_count; ++i) {
        if (g_vendor_commit_fds[i] == fd) { pthread_mutex_unlock(&g_lock); return; }
    }
    int copy = dup(fd);
    if (copy < 0) { pthread_mutex_unlock(&g_lock); return; }
    if (g_vendor_commit_fd_count < 4) {
        g_vendor_commit_fds[g_vendor_commit_fd_count++] = copy;
    } else {
        const int slot = g_vendor_commit_fd_next++ % 4;
        close(g_vendor_commit_fds[slot]);
        g_vendor_commit_fds[slot] = copy;
    }
    pthread_mutex_unlock(&g_lock);
    LOGI("xproc captured vendor commit fd=%d (pool=%d)", fd, g_vendor_commit_fd_count);
}

// Retained only for the rejected direct-commit experiment. The supported
// xproc_vendor path always passes the exact job destination fd instead.
static int vendor_pick_commit_fd(void* dst) {
    (void)dst;
    pthread_mutex_lock(&g_lock);
    int fd = -1;
    if (g_vendor_commit_fd_count >= 2) {
        const int slot = g_vendor_commit_fd_next++ % g_vendor_commit_fd_count;
        fd = dup(g_vendor_commit_fds[slot]);
    }
    pthread_mutex_unlock(&g_lock);
    return fd;
}

static int xproc_send_raw_output(int sock, int raw_fd) {
    uint32_t magic = kRawOutputMagic;
    char control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof(control));
    iovec iov{&magic, sizeof(magic)};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &raw_fd, sizeof(raw_fd));
    msg.msg_controllen = cmsg->cmsg_len;
    return sendmsg(sock, &msg, MSG_NOSIGNAL) < 0 ? -errno : 0;
}

// GraphicBuffer::flatten/unflatten wire format (frameworks/native/libs/ui).
// Sending it directly avoids importing/locking the client target in the composer,
// which gralloc.rogue rejects even though the helper can import the same GB01.
static int xproc_send_input(int fd, const CtSnap& s, const native_handle_t* handle) {
    if (!handle || handle->numFds <= 0 || handle->numFds > 128 || handle->numInts < 0 ||
        handle->numInts > 128) return -1;
    const size_t words = 13 + (size_t)handle->numInts;
    int32_t* buf = static_cast<int32_t*>(calloc(words, sizeof(int32_t)));
    if (!buf) return -1;
    buf[0] = (int32_t)kGbMagic;
    buf[1] = s.w;
    buf[2] = s.h;
    buf[3] = s.stride;
    buf[4] = s.format;
    buf[5] = 1;
    // Match the AHardwareBuffer usage bits used by the known-good SF-shaped
    // source allocation (CPU read/write rarely + GPU sampled/framebuffer).
    buf[6] = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
             AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
             AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
             AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY;
    buf[7] = 1;
    buf[10] = handle->numFds;
    buf[11] = handle->numInts;
    if (handle->numInts)
        memcpy(buf + 13, handle->data + handle->numFds, (size_t)handle->numInts * sizeof(int32_t));

    char control[CMSG_SPACE(128 * sizeof(int))];
    memset(control, 0, sizeof(control));
    iovec iov{buf, words * sizeof(int32_t)};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN((size_t)handle->numFds * sizeof(int));
    memcpy(CMSG_DATA(cmsg), handle->data, (size_t)handle->numFds * sizeof(int));
    msg.msg_controllen = cmsg->cmsg_len;
    const ssize_t sent = sendmsg(fd, &msg, MSG_NOSIGNAL);
    free(buf);
    return sent < 0 ? -errno : 0;
}

// The staging-copy skip is safe only in the fast waveform family. HD 256 and
// Regal use a different vendor buffer/compare path and can crash in processImage
// when the staging copy is omitted. Read the live property here so a mode switch
// takes effect on the next frame rather than after the knob reload interval.
static bool xproc_mode_is_fast() {
    char buf[PROP_VALUE_MAX] = {0};
    if (__system_property_get("vendor.xrz.global_refresh_mode", buf) <= 0) return false;
    const int mode = atoi(buf);
    // A2, Fast, Extreme and Hyper (0xb7). HD/HD256/Regal must stay on the full
    // vendor conversion/copy path. Never infer the current mode from the previous
    // commit: that can re-enable the helper one frame after a quality-mode switch.
    return mode == 16 || mode == 178 || mode == 179 || mode == 183;
}

// A timed-out request may still be writing the same frame's snapshot into the
// destination. Treat that destination as unavailable for the retry window; the
// vendor path then owns it. Because the helper is fed a job-bound snapshot, a
// late write is at worst the same frame, never a newer/older client target.
static void quarantine_dest_fd(int fd, uint64_t duration_ms) {
    uint64_t dev = 0, ino = 0;
    if (!fd_identity(fd, &dev, &ino)) return;
    const uint64_t until = now_ns() + duration_ms * 1000000ull;
    pthread_mutex_lock(&g_lock);
    int slot = 0;
    for (int i = 0; i < 4; ++i) {
        if (g_dest_quarantine[i].until_ns <= now_ns() ||
            (g_dest_quarantine[i].dev == dev && g_dest_quarantine[i].ino == ino)) {
            slot = i;
            break;
        }
        if (g_dest_quarantine[i].until_ns < g_dest_quarantine[slot].until_ns) slot = i;
    }
    g_dest_quarantine[slot] = DestQuarantine{ dev, ino, until };
    pthread_mutex_unlock(&g_lock);
    if (g_log >= 1)
        LOGW("xproc quarantined destination ino=%llu for %llums",
             (unsigned long long)ino, (unsigned long long)duration_ms);
}

static bool xproc_circuit_open() {
    const uint64_t retry = __atomic_load_n(&g_xproc_retry_after_ns, __ATOMIC_RELAXED);
    return retry != 0 && now_ns() < retry;
}

static bool dest_quarantined_locked(uint64_t dev, uint64_t ino) {
    const uint64_t now = now_ns();
    for (int i = 0; i < 4; ++i) {
        if (g_dest_quarantine[i].until_ns > now &&
            g_dest_quarantine[i].dev == dev && g_dest_quarantine[i].ino == ino)
            return true;
    }
    return false;
}

static bool dest_quarantined_fd(int fd) {
    uint64_t dev = 0, ino = 0;
    if (!fd_identity(fd, &dev, &ino)) return false;
    pthread_mutex_lock(&g_lock);
    const bool quarantined = dest_quarantined_locked(dev, ino);
    pthread_mutex_unlock(&g_lock);
    return quarantined;
}

static void request_helper_recovery() {
    const uint64_t now = now_ns();
    if (g_helper_recovery_set &&
        g_helper_recovery_last_ns && now - g_helper_recovery_last_ns < 2000000000ull)
        return;
    g_helper_recovery_last_ns = now;
    g_helper_recovery_set = true;
    __system_property_set("vendor.debug.hwc.shim.helper_recovery", "1");
}

// The helper writes the whole frame into the exact vendor CommitBuffer dma-buf
// resolved for this job. `mtk_commit` still receives the vendor's own unmodified
// cfg/fd; no destination-fd guess or cfg rewrite is used.
static void job_end() {
    ct_snap_release(&t_job.snap);
    if (t_job.dst_fd >= 0) close(t_job.dst_fd);
    t_job.active = false;
    t_job.matched = false;
    t_job.copy_skipped = false;
    t_job.helper_done = false;
    t_job.helper_ambiguous = false;
    t_job.fence_waited = false;
    t_job.self = nullptr;
    t_job.param = nullptr;
    t_job.ci = nullptr;
    t_job.dst_fd = -1;
    t_job.src_dev = 0;
    t_job.src_ino = 0;
    t_job.dst_dev = 0;
    t_job.dst_ino = 0;
    t_job.seq = 0;
    t_job.damage_l = 0;
    t_job.damage_t = 0;
    t_job.damage_r = -1;
    t_job.damage_b = -1;
    t_job.damage_valid = false;
}

// Resolve the exact vendor CommitBuffer fd whose GraphicBuffer is being locked
// as the processImage output. processImage selects a slot under +0x428, then
// locks CommitBuffer+0x8 with usage 0x30 before convertToGray. Matching the
// GraphicBuffer object here is stronger than guessing from pool state early.
static int vendor_resolve_job_dst_fd(void* self, void* graphic_buffer) {
    if (!self || !graphic_buffer || !g_buildid_ok) return -1;
    uint8_t* eng = static_cast<uint8_t*>(self);
    pthread_mutex_t* mtx = reinterpret_cast<pthread_mutex_t*>(eng + 0x428);
    int fd = -1;
    pthread_mutex_lock(mtx);
    static const size_t kSlots[3] = { 0x610, 0x618, 0x620 };
    for (size_t off : kSlots) {
        uint8_t* cb = *reinterpret_cast<uint8_t* const*>(eng + off);
        if (!cb) continue;
        void* gb = *reinterpret_cast<void* const*>(cb + 0x08);
        if (gb != graphic_buffer) continue;
        const int cand = *reinterpret_cast<const int32_t*>(cb + 0x18);
        if (cand >= 0) fd = dup(cand);
        break;
    }
    pthread_mutex_unlock(mtx);
    return fd;
}

// Bind the worker's current job to the exact client target named by
// OverlayPortParam+0x78 (BuildID-gated). +0x78 is the source CT fd, not the
// commit destination. If the identity does not match a snapshot we keep the
// vendor path intact instead of guessing "latest frame".
static void job_begin(void* self, void* param, void* ci) {
    job_end();
    t_job.active = true;
    t_job.self = self;
    t_job.param = param;
    t_job.ci = ci;
    if (!g_xproc_vendor || !g_buildid_ok || !param) return;

    const int src_fd = *(const int32_t*)((const uint8_t*)param + 0x78);
    if (src_fd < 0) return;
    fd_identity(src_fd, &t_job.src_dev, &t_job.src_ino);

    pthread_mutex_lock(&g_lock);
    t_job.matched = snap_hist_clone_by_fd_locked(src_fd, &t_job.snap, &t_job.seq);
    t_job.damage_l = g_pend_ct.l;
    t_job.damage_t = g_pend_ct.t;
    t_job.damage_r = g_pend_ct.r;
    t_job.damage_b = g_pend_ct.b;
    t_job.damage_valid = !rect_empty(g_pend_ct) && g_pend_ct_rects > 0;
    bool latest_fallback = false;
    if (!t_job.matched && g_snap.valid) {
        // Some vendor paths carry a layer/staging fd at +0x78 rather than the
        // CT. Bind the latest accepted CT at job start (not at conversion), so
        // a newer setClientTarget cannot swap it mid-job.
        t_job.matched = ct_snap_clone_locked(g_snap, &t_job.snap);
        t_job.seq = (uint64_t)g_snap_seq;
        latest_fallback = t_job.matched;
    }
    pthread_mutex_unlock(&g_lock);
    if (g_log >= 1 && (g_st_pi.n < 6 || (g_st_pi.n % 120) == 0))
        LOGI("xproc job param=%p ci=%p src_fd=%d src_ino=%llu snapshot=%s seq=%llu",
             param, ci, src_fd, (unsigned long long)t_job.src_ino,
             t_job.matched ? (latest_fallback ? "latest" : "matched") : "missing",
             (unsigned long long)t_job.seq);
}

// Same-frame fallback when the helper fails after we skipped the vendor staging
// copy: convert the job-bound snapshot straight into the vendor's locked
// destination buffer (`dst` in `hook_convert_to_gray`).
static bool job_direct_fallback(void* dst_ptr, int w, int h, int dst_pitch) {
    if (!t_job.matched || !t_job.snap.valid || !t_job.fence_waited ||
        !dst_ptr || w <= 0 || h <= 0 || dst_pitch < w) return false;
    if (t_job.snap.w != w || t_job.snap.h != h) return false;
    uint8_t* base = nullptr;
    size_t len = 0;
    if (!ct_map(t_job.snap, false, &base, &len)) return false;
    gray_conv((uint8_t*)dst_ptr, base, w, h, t_job.snap.stride * 4, dst_pitch,
              g_helper_onebit != 0);
    ct_unmap_sync(t_job.snap, false);
    return true;
}

static int xproc_recv_output(int fd, const CtSnap& s) {
    constexpr size_t kMaxWords = 4096;
    int32_t* buf = static_cast<int32_t*>(malloc(kMaxWords * sizeof(int32_t)));
    if (!buf) return -1;
    char control[CMSG_SPACE(128 * sizeof(int))];
    memset(control, 0, sizeof(control));
    iovec iov{buf, kMaxWords * sizeof(int32_t)};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    const ssize_t got = recvmsg(fd, &msg, 0);
    if (got < 0 || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC))) {
        free(buf);
        return -1;
    }

    cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    int fd_count = 0;
    int* received_fds = nullptr;
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
        cmsg->cmsg_len >= CMSG_LEN(0)) {
        fd_count = (int)(cmsg->cmsg_len - CMSG_LEN(0)) / (int)sizeof(int);
        received_fds = reinterpret_cast<int*>(CMSG_DATA(cmsg));
    }

    int result = -1;
    if (got >= (ssize_t)sizeof(int32_t) && (uint32_t)buf[0] == kGrayErrorMagic) {
        if (got >= (ssize_t)(6 * sizeof(int32_t)) && g_log >= 1)
            LOGW("xproc helper error status=%d native=%d", buf[2], buf[3]);
    } else if (got >= (ssize_t)(13 * sizeof(int32_t)) &&
               (uint32_t)buf[0] == kGbMagic && received_fds && fd_count > 0) {
        const int num_fds = buf[10], num_ints = buf[11];
        const size_t need = (size_t)(13 + (num_ints > 0 ? num_ints : 0)) * sizeof(int32_t);
        if (num_fds > 0 && num_fds <= 128 && num_ints >= 0 && num_ints < 4096 &&
            (size_t)got >= need && fd_count >= num_fds &&
            buf[1] == s.w && buf[2] == s.h && buf[3] >= s.w && buf[4] == 1 && buf[5] == 1) {
            result = dup(received_fds[0]);
        }
    }
    for (int i = 0; i < fd_count; ++i) close(received_fds[i]);
    if (result < 0 && g_log >= 1) {
        static int reply_logs;
        if (reply_logs++ < 8) {
            char detail[96] = {0};
            if (got >= (ssize_t)(6 * sizeof(int32_t))) {
                snprintf(detail, sizeof(detail), "words=%08x,%u,%u,%u,%u,%u",
                         buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
            }
            LOGW("xproc reply invalid got=%zd flags=0x%x fds=%d %s",
                 got, (unsigned)msg.msg_flags, fd_count, detail);
        }
    }
    free(buf);
    return result;
}

static int xproc_convert(const CtSnap& s, uint32_t mode, int* out_stride, bool raw_output,
                         void* raw_dst = nullptr, int raw_fd_override = -1,
                         bool* ambiguous = nullptr, Rect damage = { 0, 0, -1, -1 }) {
    if (out_stride) *out_stride = 0;
    if (ambiguous) *ambiguous = false;
    if (!s.valid || !s.handle) return -1;
    if (raw_output && raw_fd_override >= 0 && dest_quarantined_fd(raw_fd_override)) {
        if (ambiguous) *ambiguous = false;
        if (g_log >= 1)
            LOGW("xproc exact vendor destination is quarantined; keeping vendor converter");
        return -1;
    }
    if (xproc_circuit_open()) {
        __atomic_store_n(&g_xproc_ready, 0, __ATOMIC_RELAXED);
        return -1;
    }
    const uint64_t started = now_ns();
    ++g_xproc_attempts;
    int result_fd = -1;
    int raw_output_fd = -1;
    bool raw_sent = false;
    const char* fail_stage = "accept";
    native_handle_t* request_handle = nullptr;
    GrayRequest request{};
    Rect d = damage;
    bool partial = false;

    pthread_mutex_lock(&g_xproc_lock);
    if (!xproc_accept_locked()) goto done;
    fail_stage = "clone";
    pthread_mutex_lock(&g_lock);
    request_handle = native_handle_clone(s.handle);
    pthread_mutex_unlock(&g_lock);
    if (!request_handle) goto done;
    fail_stage = "rawfd";
    if (raw_output) {
        // The vendor path knows the exact destination fd for this job. Never
        // use the old round-robin pool there: a wrong destination feeds another
        // frame's buffer into the commit being prepared right now.
        raw_output_fd = raw_fd_override >= 0 ? dup(raw_fd_override)
                                             : vendor_pick_commit_fd(raw_dst);
        if (raw_output_fd < 0) goto done;
    }

    if (raw_output && g_xproc_damage && g_xform == 0 && !rect_empty(d) &&
        s.w > 0 && s.h > 0 && (s.w % 4) == 0) {
        if (d.l < 0) d.l = 0;
        if (d.t < 0) d.t = 0;
        if (d.r >= s.w) d.r = s.w - 1;
        if (d.b >= s.h) d.b = s.h - 1;
        const long changed = area(d);
        const long total = (long)s.w * (long)s.h;
        // A bounding union of several small rects can be mostly empty. Full
        // conversion is faster and safer once damage covers most of the frame.
        partial = changed > 0 && total > 0 && changed * 4 < total * 3;
    }
    request.magic = kGrayMagic;
    request.version = 2;
    request.width = (uint32_t)s.w;
    request.height = (uint32_t)s.h;
    request.onebit = g_helper_onebit ? 1u : 0u;
    request.reserved = raw_output ? 1u : 0u;
    request.left = partial ? (uint32_t)d.l : UINT32_MAX;
    request.top = partial ? (uint32_t)d.t : UINT32_MAX;
    request.right = partial ? (uint32_t)d.r : UINT32_MAX;
    request.bottom = partial ? (uint32_t)d.b : UINT32_MAX;
    if (partial) {
        g_xproc_partial++;
        g_xproc_partial_pixels += (uint64_t)area(d);
    } else {
        g_xproc_full++;
    }
    fail_stage = "send_header";
    if (send(g_xproc_sock, &request, sizeof(request), MSG_NOSIGNAL) != (ssize_t)sizeof(request))
        goto done;
    fail_stage = "send_input";
    if (xproc_send_input(g_xproc_sock, s, request_handle) != 0) goto done;
    fail_stage = "send_raw";
    if (raw_output && xproc_send_raw_output(g_xproc_sock, raw_output_fd) != 0) goto done;
    raw_sent = raw_output;
    fail_stage = "recv";
    result_fd = xproc_recv_output(g_xproc_sock, s);
    if (result_fd < 0) goto done;
    if (raw_output) {
        // The helper's reply acknowledges that the vendor target was populated.
        // Commit the vendor fd, never the helper's separately allocated backing.
        close(result_fd);
        result_fd = dup(raw_output_fd);
        if (result_fd < 0) goto done;
    }
    if (out_stride) *out_stride = s.w;
    stat_add(&g_st_xproc, (now_ns() - started) / 1000);

done:
    if (result_fd < 0) {
        // Fail safe: once the helper is unavailable, stop skipping the vendor's
        // staging copy. Otherwise the fallback converter reads stale data and
        // the physical panel can remain frozen even though commits still return.
        __atomic_store_n(&g_xproc_ready, 0, __ATOMIC_RELAXED);
        g_xproc_fails++;
        const int consecutive = ++g_xproc_consec_fails;
        if (raw_sent && raw_output_fd >= 0) {
            if (ambiguous) *ambiguous = true;
            quarantine_dest_fd(raw_output_fd, 8000);
        }
        if (consecutive >= 3) {
            __atomic_store_n(&g_xproc_retry_after_ns,
                             now_ns() + 2000000000ull, __ATOMIC_RELAXED);
        }
        const uint64_t last_ok = __atomic_load_n(&g_xproc_last_success_ns,
                                                 __ATOMIC_RELAXED);
        if (consecutive >= 10 &&
            (last_ok == 0 || now_ns() - last_ok > 10000000000ull)) {
            request_helper_recovery();
        }
        if (g_log >= 1 && (g_xproc_fails <= 4 || (g_xproc_fails % 20) == 0))
            LOGW("xproc convert failed stage=%s after %lluus (calls=%llu failures=%llu consecutive=%d)",
                 fail_stage, (unsigned long long)((now_ns() - started) / 1000),
                 (unsigned long long)g_st_xproc.n, (unsigned long long)g_xproc_fails,
                 consecutive);
    } else {
        g_xproc_consec_fails = 0;
        __atomic_store_n(&g_xproc_last_success_ns, now_ns(), __ATOMIC_RELAXED);
        __atomic_store_n(&g_xproc_retry_after_ns, 0, __ATOMIC_RELAXED);
        if (g_helper_recovery_set) {
            g_helper_recovery_set = false;
            __system_property_set("vendor.debug.hwc.shim.helper_recovery", "0");
        }
    }
    if (raw_output_fd >= 0) close(raw_output_fd);
    if (request_handle) native_handle_delete(request_handle);
    if (result_fd < 0) xproc_disconnect_locked();
    pthread_mutex_unlock(&g_xproc_lock);
    return result_fd;
}

// The commit we own. Returns the ioctl result, or -1 when nothing was done.
static int fast_commit(CtSnap s, Rect q, uint32_t mode, uint32_t dither, bool inplace) {
    eink_update_config c = {};
    int fmt = inplace ? 0 : (g_ct_format >= 0 ? g_ct_format : s.format);
    if (!inplace && fmt == 0) fmt = 1;                  // 0 means gray8; CT is RGBA
    c.mode = mode;
    c.left = (uint32_t)q.l; c.top = (uint32_t)q.t;
    c.right = (uint32_t)q.r; c.bottom = (uint32_t)q.b;
    c.dither = dither;
    c.fd = s.fd;
    c.format = (uint32_t)fmt;
    c.stride = (uint32_t)(inplace ? s.w : s.stride);
    c.height = (uint32_t)s.h;
    c.flag = 1;

    bool own = false;
    int xproc_fd = -1;
    int xproc_stride = 0;
    if (g_xproc) {
        xproc_fd = xproc_convert(s, mode, &xproc_stride, true);
        if (xproc_fd < 0) return -1;
        c.fd = xproc_fd;
        c.format = 0;
        c.stride = (uint32_t)(xproc_stride > 0 ? xproc_stride : s.w);
        c.height = (uint32_t)s.h;
        own = true;
    } else if (g_ownbuf && ob_alloc(s.w, s.h)) {
        int i = g_ob_next++ % g_ob_count;
        uint8_t* base = nullptr; size_t len = 0;
        if (ct_map(s, false, &base, &len)) {
            uint64_t q0 = now_ns();
            ob_sync(g_ob[i].fd, true);
            uint64_t q1 = now_ns();
            gray_conv(g_ob[i].map, base, s.w, s.h, s.stride * 4, s.w, false);
            uint64_t q2 = now_ns();
            ob_sync(g_ob[i].fd, false);
            uint64_t q3 = now_ns();
            uint64_t qus = (q2 - q1) / 1000;
            g_last_sync_us = (q1 - q0 + q3 - q2) / 1000;
            stat_add(&g_st_own, qus);
            ct_unmap_sync(s, false);
            c.fd = g_ob[i].fd;
            c.format = 0;
            c.stride = (uint32_t)s.w;
            c.height = (uint32_t)s.h;
            own = true;
        }
    }
    if (g_ownbuf && !own) return -1;

    if (xproc_fd >= 0) {
#if HAVE_DMABUF_SYNC
        struct dma_buf_sync sy = {};
        sy.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
        const int sync_ret = ioctl(xproc_fd, DMA_BUF_IOCTL_SYNC, &sy);
        if (g_log >= 2 || (g_log >= 1 && g_fast_commits < 3))
            LOGI("xproc dma-buf END/WRITE sync ret=%d errno=%d", sync_ret, errno);
#else
        (void)xproc_fd;
#endif
    }

    uint64_t t0 = now_ns();
    pthread_mutex_lock(&g_commit_lock);
    int ret = real_mtk_commit(&c);
    pthread_mutex_unlock(&g_commit_lock);
    if (xproc_fd >= 0) close(xproc_fd);
    uint64_t us = (now_ns() - t0) / 1000;
    uint64_t dt_us = 0;

    if (ret == 0) {
        const uint64_t commit_ns = now_ns();
        if (g_last_fast_commit_ns) {
            dt_us = (commit_ns - g_last_fast_commit_ns) / 1000;
            g_fast_dt_sum += dt_us;
            if (dt_us > g_fast_dt_max) g_fast_dt_max = dt_us;
        }
        g_last_fast_commit_ns = commit_ns;
        __atomic_add_fetch(&g_fast_commits, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_fast_us_sum, us, __ATOMIC_RELAXED);
        uint64_t m = __atomic_load_n(&g_fast_us_max, __ATOMIC_RELAXED);
        while (us > m && !__atomic_compare_exchange_n(&g_fast_us_max, &m, us, false,
                                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    } else {
        __atomic_add_fetch(&g_fast_fails, 1, __ATOMIC_RELAXED);
    }
    if (g_log >= 2 || (g_log >= 1 && (g_fast_commits % (uint64_t)g_log_every) == 0))
        LOGI("fast #%llu %s [%d,%d,%d,%d] mode=0x%x fmt=%d stride=%d h=%d ioctl=%lluus dt=%lluus wait=%lluus sync=%lluus ret=%d",
             (unsigned long long)g_fast_commits,
             own ? (g_xproc ? "xproc-gray" : "own-gray") : (inplace ? "gray-inplace" : "rgba"),
             q.l, q.t, q.r, q.b, mode, c.format, c.stride, c.height,
             (unsigned long long)us, (unsigned long long)dt_us,
             (unsigned long long)g_last_wait_us,
             (unsigned long long)g_last_sync_us, ret);
    return ret;
}

static void probe_vendor_commit_fd(int fd, uint32_t stride, uint32_t height) {
    static int seen[8];
    static int nseen;
    for (int i = 0; i < nseen; ++i) if (seen[i] == fd) return;
    if (nseen >= 8) return;
    seen[nseen++] = fd;
    const size_t len = (size_t)stride * height;
    void* map = mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        LOGW("vendor fd probe %d mmap %zu failed: %s", fd, len, strerror(errno));
        return;
    }
    uint8_t* copy = static_cast<uint8_t*>(malloc(len));
    if (!copy) { munmap(map, len); return; }
    const uint64_t t0 = now_ns();
    memcpy(copy, map, len);
    const uint64_t us = (now_ns() - t0) / 1000;
    uint64_t sum = 0;
    for (size_t i = 0; i < len; i += 4096) sum += copy[i];
    LOGI("vendor fd probe fd=%d bytes=%zu read=%lluus MB/s=%.1f checksum=%llu",
         fd, len, (unsigned long long)us,
         us ? (double)len / (double)us : 0.0,
         (unsigned long long)sum);
    free(copy);
    munmap(map, len);
}

// Called on the vendor's e-ink worker with a pointer to the caller's by-value
// eink_update_config, so it is safe to rewrite in place.
static int hook_mtk_commit(eink_update_config* cfg) {
    maybe_reload_knobs();
    if (cfg) {
        pthread_mutex_lock(&g_lock);
        const bool drop = g_drop_next_commit;
        if (drop) g_drop_next_commit = false;
        pthread_mutex_unlock(&g_lock);
        if (drop) {
            LOGW("dropping commit after helper/fallback failure");
            return 0;
        }
        if (g_xproc && !g_xproc_vendor) vendor_capture_commit_fd(cfg->fd);
    }
    if (!g_fast) __atomic_store_n(&g_fast_active, 0, __ATOMIC_RELAXED);
    if (__atomic_load_n(&g_fast_active, __ATOMIC_RELAXED)) {
        // Keep policy changes from E-ink Center visible to our next fast commit
        // even though the vendor's physical commit is suppressed.
        if (cfg) {
            __atomic_store_n(&g_vendor_mode, cfg->mode, __ATOMIC_RELAXED);
            __atomic_store_n(&g_last_vendor_dither, cfg->dither, __ATOMIC_RELAXED);
        }
        __atomic_add_fetch(&g_fast_skips, 1, __ATOMIC_RELAXED);
        return 0;
    }

    Rect ct, layer;
    int nct, nly;
    pthread_mutex_lock(&g_lock);
    ct = g_pend_ct; nct = g_pend_ct_rects;
    layer = g_pend_layer; nly = g_pend_layer_rects;
    g_pend_ct = g_pend_layer = Rect{ 0, 0, -1, -1 };
    g_pend_ct_rects = g_pend_layer_rects = 0;
    g_commits++;
    uint64_t t = now_ns();
    uint64_t dt_us = g_last_commit_ns ? (t - g_last_commit_ns) / 1000 : 0;
    uint64_t pre_us = g_last_present_ns ? (t - g_last_present_ns) / 1000 : 0;
    g_last_commit_ns = t;
    pthread_mutex_unlock(&g_lock);

    if (!cfg) return real_mtk_commit(cfg);

    const uint32_t vm = cfg->mode;
    Rect vr = { (int)cfg->left, (int)cfg->top, (int)cfg->right, (int)cfg->bottom };
    __atomic_store_n(&g_last_vendor_dither, cfg->dither, __ATOMIC_RELAXED);

    if (vr.r > 0 && (int)cfg->right + 1 > g_panel_w) g_panel_w = (int)cfg->right + 1;
    if (vr.b > 0 && (int)cfg->bottom + 1 > g_panel_h) g_panel_h = (int)cfg->bottom + 1;

    Rect want = (g_src == 2) ? layer : ct;
    want = xform_rect(want);
    bool has = !rect_empty(want);
    bool rewrite = (g_rect == 1) && has;
    if (rewrite && g_min_pct && area(want) >= area(vr)) rewrite = false;

    if (g_flag >= 0) cfg->flag = (uint32_t)g_flag;
    if (g_test_height > 0) cfg->height = (uint32_t)g_test_height;
    if (g_test_w > 0 && g_test_h > 0) {
        want = Rect{ 0, 0, g_test_w - 1, g_test_h - 1 };
        rewrite = true;
    }
    if (rewrite) {
        cfg->left = (uint32_t)want.l; cfg->top = (uint32_t)want.t;
        cfg->right = (uint32_t)want.r; cfg->bottom = (uint32_t)want.b;
    }
    if (g_mode >= 0) cfg->mode = (uint32_t)g_mode;
    if (g_use_ct && g_ct.valid) {
        cfg->fd = g_ct.fd;
        cfg->format = (uint32_t)(g_ct_format >= 0 ? g_ct_format : g_ct.format);
        cfg->stride = (uint32_t)g_ct.stride;
        cfg->height = (uint32_t)g_ct.h;
    }

    pthread_mutex_lock(&g_lock);
    __atomic_store_n(&g_vendor_mode, vm, __ATOMIC_RELAXED);
    g_vendor_rect = vr;
    g_new_rect = Rect{ (int)cfg->left, (int)cfg->top, (int)cfg->right, (int)cfg->bottom };
    g_new_mode = cfg->mode;
    if (rewrite) g_rewrites++;
    bool do_log = g_log >= 1 &&
                  (g_logged < 8 || (g_commits % (uint64_t)g_log_every) == 0 || g_log >= 2);
    if (do_log) g_logged++;
    pthread_mutex_unlock(&g_lock);

    uint64_t t0 = now_ns();
    pthread_mutex_lock(&g_commit_lock);
    int ret = real_mtk_commit(cfg);
    pthread_mutex_unlock(&g_commit_lock);
    if (g_vendor_fd_probe && ret == 0)
        probe_vendor_commit_fd(cfg->fd, cfg->stride, cfg->height);
    uint64_t ioctl_us = (now_ns() - t0) / 1000;

    pthread_mutex_lock(&g_lock);
    g_last_ret = ret;
    pthread_mutex_unlock(&g_lock);

    if (do_log && g_commits <= 3) {
        LOGI("vendor cfg reserved r18=[%x,%x,%x,%x] r30=[%x,%x] flag=%x",
             cfg->r18[0], cfg->r18[1], cfg->r18[2], cfg->r18[3],
             cfg->r30[0], cfg->r30[1], cfg->flag);
    }

    if (do_log) {
        LOGI("commit #%llu dt=%lluus pre=%lluus ioctl=%lluus %s mode=0x%x->0x%x area=(%d,%d,%d,%d)->(%d,%d,%d,%d) "
             "ct=(%d,%d,%d,%d)/%d layer=(%d,%d,%d,%d)/%d fd=%d fmt=0x%x stride=%d h=%d ret=%d",
             (unsigned long long)g_commits, (unsigned long long)dt_us,
             (unsigned long long)pre_us, (unsigned long long)ioctl_us,
             rewrite ? "RECT" : "pass", vm, cfg->mode,
             vr.l, vr.t, vr.r, vr.b,
             (int)cfg->left, (int)cfg->top, (int)cfg->right, (int)cfg->bottom,
             ct.l, ct.t, ct.r, ct.b, nct, layer.l, layer.t, layer.r, layer.b, nly,
             cfg->fd, cfg->format, cfg->stride, cfg->height, ret);
    }
    return ret;
}

// gralloc buffer at 1.8 GB/s through the mapper, 2.2 ms for a 1440x720 frame.
// The probe measures whether the real client target behaves the same, read-only,
// once a second, and verifies both mappings produce identical gray output.
//
// Knob: vendor.debug.hwc.shim.ctprobe=N  (N = attempts per process, 0 = off)
//
// MEASURED 2026-11-24 (gpu-gray/ --xproc): a foreign handle imported in a second
// process and locked there reads 4 MB in 2.2 ms (1.7 GB/s, lock itself 0.13 ms).
// Without importBuffer, lock() faults inside gralloc.rogue.so -- the first probe
// crashed the composer that way (SIGSEGV at 0x8, MapperImpl_v4_0::lock).
static void* (*p_gbm_instance)(void);
static int32_t (*p_gbm_import)(void*, const native_handle_t*, uint32_t, uint32_t, uint32_t,
                               int32_t, uint64_t, uint32_t, const native_handle_t**);
static int32_t (*p_gbm_free)(void*, const native_handle_t*);
static int32_t (*p_gbm_lock)(void*, const native_handle_t*, uint32_t, const void*, void**,
                             int32_t*, int32_t*);
static int32_t (*p_gbm_unlock)(void*, const native_handle_t*);
static int g_libui_state;                       // 0 untried, 1 ok, -1 unavailable

#define GRALLOC_USAGE_SW_READ_RARELY  0x00000001u
#define GRALLOC_USAGE_SW_WRITE_RARELY 0x00000002u
#define GRALLOC_USAGE_HW_TEXTURE      0x00000100u
#define GRALLOC_USAGE_HW_RENDER       0x00000200u

static bool load_libui() {
    if (g_libui_state) return g_libui_state > 0;
    g_libui_state = -1;
    // The composer is a vendor process and already has the VNDK libui mapped
    // (VNDK v31 on this build). Its GraphicBufferMapper is the same class AOSP
    // uses for consumer-side buffers; /system's newer libui has another ABI.
    void* h = dlopen("/apex/com.android.vndk.v31/lib64/libui.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libui.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("/system/lib64/libui.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) { LOGE("mapper: libui not loadable: %s", dlerror()); return false; }
    *(void**)&p_gbm_instance = dlsym(h, "_ZN7android9SingletonINS_19GraphicBufferMapperEE11getInstanceEv");
    *(void**)&p_gbm_import = dlsym(
            h, "_ZN7android19GraphicBufferMapper12importBufferEPK13native_handlejjjimjPS3_");
    *(void**)&p_gbm_free = dlsym(h, "_ZN7android19GraphicBufferMapper10freeBufferEPK13native_handle");
    *(void**)&p_gbm_lock = dlsym(
            h, "_ZN7android19GraphicBufferMapper4lockEPK13native_handlejRKNS_4RectEPPvPiS9_");
    *(void**)&p_gbm_unlock = dlsym(h, "_ZN7android19GraphicBufferMapper6unlockEPK13native_handle");
    if (!p_gbm_instance || !p_gbm_import || !p_gbm_free || !p_gbm_lock || !p_gbm_unlock) {
        LOGE("mapper: libui loaded but symbols missing (inst=%p imp=%p free=%p lock=%p unl=%p)",
             (void*)p_gbm_instance, (void*)p_gbm_import, (void*)p_gbm_free, (void*)p_gbm_lock,
             (void*)p_gbm_unlock);
        return false;
    }
    g_libui_state = 1;
    LOGI("mapper: libui ok (inst=%p imp=%p free=%p lock=%p unl=%p)", (void*)p_gbm_instance,
         (void*)p_gbm_import, (void*)p_gbm_free, (void*)p_gbm_lock, (void*)p_gbm_unlock);
    return true;
}

static uint8_t* g_probe_raw_map;
static size_t   g_probe_raw_len;
static int      g_probe_raw_fd = -1;
static uint8_t* g_probe_ref;
static uint8_t* g_probe_chk;

// Persistent import of the current client target: importBuffer pins the buffer
// and lock() gives the cached CPU mapping (2.2 ms for 4 MB, vs 80 ms for a raw
// mmap of the same dma-buf). Used by ctsrc so the converter can read the client
// target at speed.
static void* g_gbm;
static const native_handle_t* g_ct_imp;
static buffer_handle_t g_ct_imp_for;

static void ct_import(buffer_handle_t target) {
    if (!load_libui()) return;
    if (!g_gbm) g_gbm = p_gbm_instance ? p_gbm_instance() : nullptr;
    if (!g_gbm || !p_gbm_import) return;
    if (target == g_ct_imp_for) return;                 // same buffer, already pinned
    if (!target || g_ct.w <= 0 || g_ct.h <= 0) return;
    int32_t fmt = (g_ct.format > 0 && g_ct.format < 0x100) ? g_ct.format : 1;
    uint64_t iusage = GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_RENDER |
                      GRALLOC_USAGE_SW_READ_RARELY | GRALLOC_USAGE_SW_WRITE_RARELY;
    const native_handle_t* imp = nullptr;
    int32_t st = p_gbm_import(g_gbm, target, (uint32_t)g_ct.w, (uint32_t)g_ct.h, 1, fmt,
                              iusage, (uint32_t)g_ct.stride, &imp);
    if (st == 0 && imp) {
        // Deliberately never freed: SurfaceFlinger calls setClientTarget while
        // the e-ink worker may be converting from the previous import. Releasing
        // it here is a use-after-free (it crashed the composer); the imports are
        // a handful of pinned buffers, so leaking them is the correct trade.
        pthread_mutex_lock(&g_lock);
        g_ct_imp = imp;
        g_ct_imp_for = target;
        pthread_mutex_unlock(&g_lock);
    } else if (g_log >= 1) {
        LOGE("ct_import: importBuffer st=%d (%dx%d fmt=0x%x)", st, g_ct.w, g_ct.h, fmt);
    }
}

static void ct_probe(buffer_handle_t target, int fence) {
    static uint64_t next_ns;
    static int attempts;
    uint64_t now = now_ns();
    if (attempts >= g_ctprobe) return;
    if (now < next_ns) return;
    next_ns = now + 1000000000ull;
    if (!target || !g_ct.valid || g_ct.w <= 0 || g_ct.h <= 0) return;
    if (!g_power_on) return;                    // no client target while the panel is off
    if (!load_libui()) return;
    attempts++;

    int w = g_ct.w, h = g_ct.h, stride = g_ct.stride;
    size_t need = (size_t)w * (size_t)h;
    if (!g_probe_ref) {
        g_probe_ref = (uint8_t*)malloc(need);
        g_probe_chk = (uint8_t*)malloc(need);
    }
    if (!g_probe_ref || !g_probe_chk) return;

    // the frame's producer fence decides when the contents are final; wait on a
    // copy so SurfaceFlinger still owns the original
    int f = (fence >= 0) ? dup(fence) : -1;
    if (f >= 0) { wait_fence(f, 100); close(f); }

    // 1. the mapper (gralloc HAL): importBuffer, then lock -- the AOSP consumer
    //    flow. Importing is mandatory for a handle this process did not allocate.
    void* self = p_gbm_instance ? p_gbm_instance() : nullptr;
    bool mapper_ok = false;
    if (self && p_gbm_import) {
        int32_t fmt = (g_ct.format > 0 && g_ct.format < 0x100) ? g_ct.format : 1;
        uint64_t iusage = GRALLOC_USAGE_HW_TEXTURE | GRALLOC_USAGE_HW_RENDER |
                          GRALLOC_USAGE_SW_READ_RARELY | GRALLOC_USAGE_SW_WRITE_RARELY;
        const native_handle_t* imp = nullptr;
        uint64_t t0 = now_ns();
        int32_t st = p_gbm_import(self, target, (uint32_t)w, (uint32_t)h, 1, fmt, iusage,
                                  (uint32_t)stride, &imp);
        uint64_t t1 = now_ns();
        LOGI("ctprobe: import st=%d imp=%p fmt=0x%x usage=0x%llx %dx%d stride=%d in %lldus",
             st, (const void*)imp, fmt, (unsigned long long)iusage, w, h, stride,
             (long long)((t1 - t0) / 1000));
        if (st == 0 && imp) {
            Rect b = { 0, 0, w, h };
            void* addr = nullptr;
            int32_t bpp = 0, bps = 0;
            uint64_t t2 = now_ns();
            int32_t ls = p_gbm_lock(self, imp, GRALLOC_USAGE_SW_READ_RARELY, &b, &addr, &bpp, &bps);
            uint64_t t3 = now_ns();
            if (ls == 0 && addr) {
                int pitch = bps > 0 ? bps : w * 4;
                gray_conv(g_probe_ref, (const uint8_t*)addr, w, h, pitch, w, false);
                uint64_t t4 = now_ns();
                p_gbm_unlock(self, imp);
                mapper_ok = true;
                LOGI("ctprobe: MAPPER lock=%lldus conv=%lldus pitch=%d bpp=%d",
                     (long long)((t3 - t2) / 1000), (long long)((t4 - t3) / 1000), pitch, bpp);
            } else {
                LOGE("ctprobe: MAPPER lock st=%d after %lldus", ls, (long long)((t3 - t2) / 1000));
            }
            p_gbm_free(self, imp);
        }
    }

    // 2. the raw mmap of the same dma-buf: what the shim does today
    if (g_probe_raw_fd != g_ct.fd || !g_probe_raw_map) {
        if (g_probe_raw_map) { munmap(g_probe_raw_map, g_probe_raw_len); g_probe_raw_map = nullptr; }
        size_t len = (size_t)stride * 4 * (size_t)(g_ct.vstride > h ? g_ct.vstride : h);
        void* p = mmap(nullptr, len, PROT_READ, MAP_SHARED, g_ct.fd, 0);
        if (p != MAP_FAILED) {
            g_probe_raw_map = (uint8_t*)p; g_probe_raw_len = len; g_probe_raw_fd = g_ct.fd;
        } else {
            LOGE("ctprobe: raw mmap failed: %s", strerror(errno));
        }
    }
    if (g_probe_raw_map) {
        uint64_t t0 = now_ns();
        gray_conv(g_probe_chk, g_probe_raw_map, w, h, stride * 4, w, false);
        uint64_t t1 = now_ns();
        int same = (mapper_ok && memcmp(g_probe_ref, g_probe_chk, need) == 0);
        LOGI("ctprobe: RAW    conv=%lldus %dx%d stride=%d same=%s",
             (long long)((t1 - t0) / 1000), w, h, stride,
             mapper_ok ? (same ? "yes" : "NO") : "n/a");
    }
}

// ---------------------------------------------------------------- HWC2 hooks

static int32_t hook_setClientTarget(hwc2_device_t* dev, hwc2_display_t display,
                                    buffer_handle_t target, int32_t fence,
                                    int32_t dataspace, hwc_region_t damage) {
    maybe_reload_knobs();
    CtSnap candidate;
    uint64_t snap_dev = 0, snap_ino = 0;
    const bool candidate_ok =
        snap_build(target, fence, &candidate, &snap_dev, &snap_ino);
    if (g_ctprobe) ct_probe(target, fence);
    const int32_t ret =
        ((PFN_setClientTarget)g_real_fn[FN_SET_CLIENT_TARGET])(dev, display, target,
                                                               fence, dataspace, damage);
    if (ret != 0) {
        if (candidate_ok) ct_snap_release(&candidate);
        return ret;
    }
    ct_query(target);
    snap_publish(&candidate, candidate_ok, snap_dev, snap_ino);
    if (g_log >= 1 && g_present < 6) {
        char fds[96] = {0};
        if (target) {
            int n = target->numFds > 0 ? target->numFds : 0;
            if (n > 4) n = 4;
            for (int i = 0; i < n; i++)
                snprintf(fds + strlen(fds), sizeof(fds) - strlen(fds), " %d", target->data[i]);
        }
        LOGI("setClientTarget d=%llu h=%p fds[%d]%s ints=%d ds=0x%x damage=%zu",
             (unsigned long long)display, (void*)target,
             target ? target->numFds : -1, fds, target ? target->numInts : -1,
             dataspace, damage.numRects);
    }
    if (damage.numRects && damage.rects) {
        pthread_mutex_lock(&g_lock);
        for (size_t i = 0; i < damage.numRects; i++) {
            const hwc_rect_t& r = damage.rects[i];
            union_rect(g_pend_ct, r.left, r.top, r.right, r.bottom);
            union_rect(g_last_ct, r.left, r.top, r.right, r.bottom);
        }
        g_pend_ct_rects += (int)damage.numRects;
        pthread_mutex_unlock(&g_lock);
        if (g_log >= 2) {
            char buf[512] = {0};
            int n = 0;
            for (size_t i = 0; i < damage.numRects && n < (int)sizeof(buf) - 32; i++) {
                const hwc_rect_t& r = damage.rects[i];
                n += snprintf(buf + n, sizeof(buf) - n, "(%d,%d,%d,%d)",
                              r.left, r.top, r.right, r.bottom);
            }
            LOGI("damage ct n=%zu %s", damage.numRects, buf);
        }
    }
    if (g_ctsrc) ct_import(target);
    if (g_log >= 1 && g_present < 8)
        LOGI("setClientTarget d=%llu handle=%p -> ct fd=%d %dx%d stride=%d vstride=%d fmt=0x%x",
             (unsigned long long)display, (void*)target, g_ct.fd, g_ct.w, g_ct.h,
             g_ct.stride, g_ct.vstride, g_ct.format);
    return ret;
}

static int32_t hook_setLayerSurfaceDamage(hwc2_device_t* dev, hwc2_display_t display,
                                          hwc2_layer_t layer, hwc_region_t damage) {
    maybe_reload_knobs();
    if (damage.numRects && damage.rects) {
        pthread_mutex_lock(&g_lock);
        for (size_t i = 0; i < damage.numRects; i++) {
            const hwc_rect_t& r = damage.rects[i];
            union_rect(g_pend_layer, r.left, r.top, r.right, r.bottom);
            union_rect(g_last_layer, r.left, r.top, r.right, r.bottom);
        }
        g_pend_layer_rects += (int)damage.numRects;
        pthread_mutex_unlock(&g_lock);
    } else if (g_xproc) {
        // A full invalidate is commonly encoded as zero explicit HWC2 rects.
        // The helper path needs a commit trigger even when the list is empty.
        pthread_mutex_lock(&g_lock);
        union_rect(g_pend_layer, 0, 0, g_panel_w - 1, g_panel_h - 1);
        union_rect(g_last_layer, 0, 0, g_panel_w - 1, g_panel_h - 1);
        g_pend_layer_rects++;
        pthread_mutex_unlock(&g_lock);
    }
    if (g_log >= 2) {
        char buf[512] = {0};
        int n = 0;
        if (damage.rects) {
            for (size_t i = 0; i < damage.numRects && n < (int)sizeof(buf) - 32; i++)
                n += snprintf(buf + n, sizeof(buf) - n, "(%d,%d,%d,%d)",
                              damage.rects[i].left, damage.rects[i].top,
                              damage.rects[i].right, damage.rects[i].bottom);
        }
        LOGI("damage layer=%llu n=%zu %s", (unsigned long long)layer, damage.numRects, buf);
    }
    return ((PFN_setLayerSurfaceDamage)g_real_fn[FN_SET_LAYER_SURFACE_DAMAGE])(dev, display, layer, damage);
}

static int32_t hook_setLayerCompositionType(hwc2_device_t* dev, hwc2_display_t display,
                                            hwc2_layer_t layer, int32_t type) {
    // 0 CLIENT, 1 DEVICE, 2 SOLID_COLOR, 3 CURSOR, 4 SIDEBAND
    pthread_mutex_lock(&g_lock);
    if (type == 0) g_nclient++; else if (type == 1) g_ndevice++;
    pthread_mutex_unlock(&g_lock);
    LOGV("setLayerCompositionType layer=%llu type=%d", (unsigned long long)layer, type);
    return ((PFN_setLayerCompositionType)g_real_fn[FN_SET_LAYER_COMPOSITION_TYPE])(dev, display, layer, type);
}

static int32_t hook_setPowerMode(hwc2_device_t* dev, hwc2_display_t display, int32_t mode) {
    int32_t e = ((PFN_setPowerMode)g_real_fn[FN_SET_POWER_MODE])(dev, display, mode);
    // 0 OFF, 1 DOZE, 2 DOZE_SUSPEND, 3 ON. While the panel is off the vendor owns
    // the shutdown path (it may commit the last frame); fast mode resumes on ON.
    g_power_on = (mode == 3);
    if (!g_power_on) __atomic_store_n(&g_fast_active, 0, __ATOMIC_RELAXED);
    if (g_log >= 1) LOGI("setPowerMode mode=%d power_on=%d", mode, g_power_on);
    return e;
}

static int32_t hook_setLayerDisplayFrame(hwc2_device_t* dev, hwc2_display_t display,
                                         hwc2_layer_t layer, hwc_rect_t frame) {
    LOGV("setLayerDisplayFrame layer=%llu frame=(%d,%d,%d,%d)",
         (unsigned long long)layer, frame.left, frame.top, frame.right, frame.bottom);
    return ((PFN_setLayerDisplayFrame)g_real_fn[FN_SET_LAYER_DISPLAY_FRAME])(dev, display, layer, frame);
}

static int32_t hook_setLayerTransform(hwc2_device_t* dev, hwc2_display_t display,
                                      hwc2_layer_t layer, int32_t tr) {
    LOGV("setLayerTransform layer=%llu transform=%d", (unsigned long long)layer, tr);
    return ((PFN_setLayerTransform)g_real_fn[FN_SET_LAYER_TRANSFORM])(dev, display, layer, tr);
}

static int32_t hook_createLayer(hwc2_device_t* dev, hwc2_display_t display, hwc2_layer_t* out) {
    int32_t e = ((PFN_createLayer)g_real_fn[FN_CREATE_LAYER])(dev, display, out);
    if (e == 0) { pthread_mutex_lock(&g_lock); g_nlayers++; pthread_mutex_unlock(&g_lock); }
    return e;
}

static int32_t hook_destroyLayer(hwc2_device_t* dev, hwc2_display_t display, hwc2_layer_t layer) {
    pthread_mutex_lock(&g_lock);
    if (g_nlayers > 0) g_nlayers--;
    pthread_mutex_unlock(&g_lock);
    return ((PFN_destroyLayer)g_real_fn[FN_DESTROY_LAYER])(dev, display, layer);
}

static int32_t hook_presentDisplay(hwc2_device_t* dev, hwc2_display_t display, int32_t* outFence) {
    maybe_reload_knobs();

    // Snapshot what SF is presenting and the damage it brought, before the
    // vendor's worker can consume either.
    bool fast = (g_fast != 0);
    SnapLease snap_lease;
    CtSnap& s = snap_lease.snap;
    Rect want = { 0, 0, -1, -1 };
    int seq = 0;
    if (fast) {
        pthread_mutex_lock(&g_lock);
        ct_snap_clone_locked(g_snap, &s);
        seq = g_snap_seq;
        want = (g_src == 2) ? g_pend_layer : g_pend_ct;
        if (rect_empty(want)) want = g_pend_layer;   // ct damage is always empty here
        pthread_mutex_unlock(&g_lock);
    }

    int32_t fence = -1;
    int32_t e = ((PFN_presentDisplay)g_real_fn[FN_PRESENT_DISPLAY])(dev, display,
                                                                   outFence ? &fence : nullptr);
    if (outFence) *outFence = fence;
    if (outFence && (g_nofence || (fast && s.valid))) {
        // In fast mode the commit has already happened on this thread by the time
        // present returns, so the vendor's asynchronous retire fence is both
        // meaningless and (with vend_hz) dangerously late: never let SF wait on it.
        if (fence >= 0) close(fence);
        *outFence = -1;
    }
    pthread_mutex_lock(&g_lock);
    g_present++;
    int nl = g_nlayers, nc = g_nclient, nd = g_ndevice;
    g_nclient = g_ndevice = 0;
    uint64_t n = g_present;
    pthread_mutex_unlock(&g_lock);
    g_last_present_ns = now_ns();

    if (g_log >= 2 || (g_log >= 1 && n <= 6))
        LOGI("present #%llu e=%d layers=%d client=%d device=%d fast=%d", (unsigned long long)n, e, nl, nc, nd, fast);

    // ------------------------------------------------------------ fast commit
    uint64_t tw0 = now_ns();
    // We are on SurfaceFlinger's present thread. The frame is complete, the
    // client target is the final composited image, and its acquire fence tells
    // us when the GPU is done with it. Commit it ourselves; the vendor's worker
    // is asynchronous and would only burn ~32 ms re-converting it.
    if (fast && s.valid && seq != 0) {
        if (s.w != g_panel_w || s.h != g_panel_h) {
            // The client target must already be in the controller's 1440x720 space.
            // If it is the 720x1440 display instead, the kernel would need a
            // transpose it cannot do: leave this to the vendor.
            static bool warned = false;
            if (!warned) {
                warned = true;
                LOGW("client target %dx%d != controller %dx%d; fast path stays off (vendor commits)",
                     s.w, s.h, g_panel_w, g_panel_h);
            }
            return e;
        }
        Rect q = xform_rect(want);
        const bool inplace = (g_inplace != 0);
        bool dirty = !rect_empty(q);

        uint64_t now = now_ns();
        bool rate_ok = true;
        if (g_fast_hz > 0 && g_last_fast_ns &&
            now - g_last_fast_ns < 1000000000ull / (uint64_t)g_fast_hz) rate_ok = false;

        // Nothing to send and nothing to look at: no panel update at all. This is
        // the biggest power/latency win on an e-ink panel. Only suppress the
        // vendor once we have actually committed a frame ourselves.
        if (!dirty && !g_fast_diff && g_fast_idle) {
            g_consumed_seq = seq;
            if (__atomic_load_n(&g_fast_commits, __ATOMIC_RELAXED) > 0)
                __atomic_store_n(&g_fast_active, 1, __ATOMIC_RELAXED);
            __atomic_add_fetch(&g_fast_skips, 1, __ATOMIC_RELAXED);
            pthread_mutex_lock(&g_lock);
            g_pend_ct = g_pend_layer = Rect{ 0, 0, -1, -1 };
            pthread_mutex_unlock(&g_lock);
        } else if (!rate_ok) {
            // keep the damage pending for the next present
        } else if (!wait_fence(s.fence, g_fast_wait_ms)) {
            g_consumed_seq = seq;      // do not stall every present on the same buffer
            if (g_log >= 1 && (g_fast_fails % 20) == 0)
                LOGW("client target fence not signalled in %dms; leaving this frame to the vendor",
                     g_fast_wait_ms);
            __atomic_store_n(&g_fast_active, 0, __ATOMIC_RELAXED);
        } else {
            if (!inplace && g_fast_diff) {
                uint8_t* base = nullptr; size_t len = 0;
                if (ct_map(s, false, &base, &len)) {
                    Rect d;
                    dirty = diff_bbox(s, base, s.stride * 4, s.h, &d);
                    if (dirty) q = d;      // diff is already in controller space
                    ct_unmap_sync(s, false);
                }
            }
            if (!dirty && !g_fast_idle) q = Rect{ 0, 0, g_panel_w - 1, g_panel_h - 1 };
            clamp_panel(q);

            if (!dirty && g_fast_idle) {
                g_consumed_seq = seq;
                if (__atomic_load_n(&g_fast_commits, __ATOMIC_RELAXED) > 0)
                    __atomic_store_n(&g_fast_active, 1, __ATOMIC_RELAXED);
                __atomic_add_fetch(&g_fast_skips, 1, __ATOMIC_RELAXED);
                pthread_mutex_lock(&g_lock);
                g_pend_ct = g_pend_layer = Rect{ 0, 0, -1, -1 };
                pthread_mutex_unlock(&g_lock);
            } else {
                if (inplace) inplace_gray(s, q.t, q.b);
                uint32_t vendor_mode = __atomic_load_n(&g_vendor_mode, __ATOMIC_RELAXED);
                uint32_t vendor_dither = __atomic_load_n(&g_last_vendor_dither, __ATOMIC_RELAXED);
                uint32_t selected_mode = g_global_mode >= 0 ? (uint32_t)g_global_mode
                                      : (vendor_mode != 0xffffffff ? vendor_mode : 0x11);
                uint32_t mode = g_mode >= 0 ? (uint32_t)g_mode : selected_mode;
                uint32_t dith = g_dither >= 0 ? (uint32_t)g_dither
                              : (inplace ? 0
                                 : (vendor_dither != 0xffffffff ? vendor_dither : 0));
                int ret = fast_commit(s, q, mode, dith, inplace);
                if (ret == 0) {
                    g_consumed_seq = seq;
                    g_last_fast_ns = now;
                    __atomic_store_n(&g_fast_active, 1, __ATOMIC_RELAXED);
                    pthread_mutex_lock(&g_lock);
                    g_pend_ct = g_pend_layer = Rect{ 0, 0, -1, -1 };
                    pthread_mutex_unlock(&g_lock);
                } else {
                    if (g_log >= 1 && (g_fast_fails % 20) == 1)
                        LOGW("fast commit ret=%d (fmt=%d stride=%d h=%d); vendor stays in charge",
                             ret, g_ct_format >= 0 ? g_ct_format : s.format,
                             inplace ? s.w : s.stride, s.h);
                    __atomic_store_n(&g_fast_active, 0, __ATOMIC_RELAXED);
                }
            }
        }
    }
    if (fast && g_log >= 1 && (g_present % 10) == 0)
        LOGI("present window %lluus (present #%llu fast=%d valid=%d seq=%d target=%dx%d)",
             (unsigned long long)((now_ns() - tw0) / 1000), (unsigned long long)g_present,
             fast, s.valid, seq, s.w, s.h);
    return e;
}

static int render_summary(char* buf, size_t cap) {
    pthread_mutex_lock(&g_lock);
    int n = snprintf(buf, cap,
        "\n[hwcshim] mt6765 rect=%d mode=%d src=%d xform=%d panel=%dx%d display=%dx%d\n"
        "[hwcshim] present=%llu commits=%llu rewrites=%llu\n"
        "[hwcshim] last vendor: mode=0x%x area=(%d,%d,%d,%d)\n"
        "[hwcshim] last commit: mode=0x%x area=(%d,%d,%d,%d) ret=%d\n"
        "[hwcshim] last damage: ct=(%d,%d,%d,%d) layer=(%d,%d,%d,%d)\n"
        "[hwcshim] fast=%d active=%d inplace=%d diff=%d vend_hz=%d skipconv=%d commits=%llu skips=%llu fails=%llu "
        "ioctl avg=%lluus max=%lluus dt avg=%lluus max=%lluus\n"
        "[hwcshim] xproc=%d calls=%llu failures=%llu avg=%lluus max=%lluus partial=%llu full=%llu partial_pixels=%llu\n"
        "[hwcshim] client target: fd=%d %dx%d stride=%d vstride=%d fmt=0x%x fence=%d\n"
        "[hwcshim] exact dst_fd=%d fd_redirect=OFF persistent_socket=ON\n",
        g_rect, g_mode, g_src, g_xform, g_panel_w, g_panel_h, g_display_w, g_display_h,
        (unsigned long long)g_present, (unsigned long long)g_commits, (unsigned long long)g_rewrites,
        __atomic_load_n(&g_vendor_mode, __ATOMIC_RELAXED), g_vendor_rect.l, g_vendor_rect.t, g_vendor_rect.r, g_vendor_rect.b,
        g_new_mode, g_new_rect.l, g_new_rect.t, g_new_rect.r, g_new_rect.b, g_last_ret,
        g_last_ct.l, g_last_ct.t, g_last_ct.r, g_last_ct.b,
        g_last_layer.l, g_last_layer.t, g_last_layer.r, g_last_layer.b,
        g_fast, g_fast_active, g_inplace, g_fast_diff, g_vend_hz, g_skipconv,
        (unsigned long long)g_fast_commits, (unsigned long long)g_fast_skips,
        (unsigned long long)g_fast_fails,
        (unsigned long long)(g_fast_commits ? g_fast_us_sum / g_fast_commits : 0),
        (unsigned long long)g_fast_us_max,
        (unsigned long long)(g_fast_commits > 1 ? g_fast_dt_sum / (g_fast_commits - 1) : 0),
        (unsigned long long)g_fast_dt_max,
        g_xproc, (unsigned long long)g_st_xproc.n, (unsigned long long)g_xproc_fails,
        (unsigned long long)stat_avg(&g_st_xproc), (unsigned long long)g_st_xproc.max_us,
        (unsigned long long)g_xproc_partial, (unsigned long long)g_xproc_full,
        (unsigned long long)g_xproc_partial_pixels,
        g_snap.fd, g_snap.w, g_snap.h, g_snap.stride, g_snap.vstride, g_snap.format, g_snap.fence,
        t_job.dst_fd);
    pthread_mutex_unlock(&g_lock);
    return n;
}

static void hook_dump(hwc2_device_t* dev, uint32_t* outSize, char* outBuffer) {
    PFN_dump real = (PFN_dump)g_real_fn[FN_DUMP];
    char ours[1024];
    int n = render_summary(ours, sizeof(ours));
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(ours)) n = (int)sizeof(ours) - 1;
    if (!outBuffer) {
        uint32_t len = 0;
        if (real) real(dev, &len, nullptr);
        *outSize = len + (uint32_t)n;
        return;
    }
    uint32_t cap = *outSize, realLen = 0;
    if (real) {
        realLen = cap > (uint32_t)n ? cap - (uint32_t)n : 0;
        real(dev, &realLen, outBuffer);
        if (realLen > cap) realLen = cap;
    }
    uint32_t room = cap - realLen;
    uint32_t copy = (uint32_t)n < room ? (uint32_t)n : room;
    memcpy(outBuffer + realLen, ours, copy);
    *outSize = realLen + copy;
}

// ---------------------------------------------------------------- vendor hooks

// OverlayEngine::processImage(OverlayPortParam*, CommitInfo*) and
// convertLayerToGray(...) are member functions: the first argument is `this`.
// Timing them is the experiment that proves where the 32 ms goes; the fast path
// then removes the need for them entirely.
static __thread uint64_t t_pi_start;

// The vendor's processImage sometimes calls memcmp with a sentinel pointer
// (0xffffffffffffffff) for its 4 MB staging comparison after the helper has
// skipped the copy. Entering libc then faults. Reject only clearly invalid
// large-buffer pointers; all normal comparisons go straight to libc.
static int hook_memcmp(const void* a, const void* b, size_t n) {
    const uintptr_t pa = reinterpret_cast<uintptr_t>(a);
    const uintptr_t pb = reinterpret_cast<uintptr_t>(b);
    if (n >= 262144 && (pa < 0x10000u || pb < 0x10000u ||
                       pa == UINTPTR_MAX || pb == UINTPTR_MAX)) {
        static int logged;
        if (!logged) {
            logged = 1;
            LOGW("guarded invalid memcmp a=%p b=%p n=%zu", a, b, n);
        }
        return 1;
    }
    return real_memcmp(a, b, n);
}

static int hook_sync_wait(int fd, int timeout_ms) {
    uint64_t t0 = now_ns();
    int r = real_sync_wait(fd, timeout_ms);
    uint64_t us = (now_ns() - t0) / 1000;
    stat_add(&g_st_sync, us);
    if (g_log >= 1 && (g_st_sync.n % 20) == 0)
        LOGI("sync_wait %lluus (calls=%llu avg=%lluus max=%lluus) ret=%d",
             (unsigned long long)us, (unsigned long long)g_st_sync.n,
             (unsigned long long)stat_avg(&g_st_sync), (unsigned long long)g_st_sync.max_us, r);
    return r;
}

static int hook_gb_lock(void* self, uint32_t usage, void** out, int32_t* a, int32_t* b) {
    uint64_t t0 = now_ns();
    int r = real_gb_lock(self, usage, out, a, b);
    uint64_t us = (now_ns() - t0) / 1000;
    stat_add(&g_st_gblock, us);
    if (r == 0 && usage == 0x30 && g_xproc_vendor && t_job.active) {
        const int fd = vendor_resolve_job_dst_fd(t_job.self, self);
        if (fd >= 0) {
            if (t_job.dst_fd >= 0) close(t_job.dst_fd);
            t_job.dst_fd = fd;
            fd_identity(fd, &t_job.dst_dev, &t_job.dst_ino);
            if (g_log >= 1 && g_st_gray.n <= 4)
                LOGI("xproc exact output GraphicBuffer matched dst_fd=%d", fd);
        }
    }
    if (g_log >= 1 && (g_st_gblock.n % 40) == 0)
        LOGI("GraphicBuffer::lock usage=0x%x %lluus (calls=%llu avg=%lluus max=%lluus) ret=%d",
             usage, (unsigned long long)us, (unsigned long long)g_st_gblock.n,
             (unsigned long long)stat_avg(&g_st_gblock),
             (unsigned long long)g_st_gblock.max_us, r);
    return r;
}

static void* hook_memcpy(void* dst, const void* src, size_t n) {
    uint64_t t0 = now_ns();
    if (g_damagecopy && n == 4147200) {
        static int damagecopy_probe;
        if (g_log >= 1 && (damagecopy_probe++ < 8 || (g_st_memcpy.n % 20) == 0)) {
            Rect pq;
            pthread_mutex_lock(&g_lock);
            pq = g_pend_ct;
            pthread_mutex_unlock(&g_lock);
            LOGI("damagecopy probe pending=(%d,%d,%d,%d)/%d", pq.l, pq.t, pq.r, pq.b,
                 g_pend_ct_rects);
        }
        // The vendor normally reads the full uncached 1440x720 RGBA client target
        // into its cached staging buffer. SF now supplies valid CT damage, so keep
        // the previous staging contents outside the damaged rectangle and copy
        // only the changed pixels. The vendor's converter/diff then sees a complete
        // frame, but the expensive uncached read is proportional to damage.
        Rect q;
        pthread_mutex_lock(&g_lock);
        q = g_pend_ct;
        pthread_mutex_unlock(&g_lock);
        constexpr int W = 1440, H = 720;
        constexpr size_t PITCH = (size_t)W * 4;
        if (!rect_empty(q)) {
            if (q.l < 0) q.l = 0;
            if (q.t < 0) q.t = 0;
            if (q.r >= W) q.r = W - 1;
            if (q.b >= H) q.b = H - 1;
            const int x0 = q.l * 4;
            const int x1 = (q.r + 1) * 4;
            const size_t bytes = (size_t)(x1 - x0) * (size_t)(q.b - q.t + 1);
            if (bytes > 0 && bytes < n) {
                for (int y = q.t; y <= q.b; y++) {
                    real_memcpy((uint8_t*)dst + (size_t)y * PITCH + x0,
                                (const uint8_t*)src + (size_t)y * PITCH + x0,
                                (size_t)(x1 - x0));
                }
                __atomic_add_fetch(&g_damagecopy_hits, 1, __ATOMIC_RELAXED);
                __atomic_add_fetch(&g_damagecopy_bytes, bytes, __ATOMIC_RELAXED);
                __atomic_add_fetch(&g_damagecopy_full_bytes, n, __ATOMIC_RELAXED);
                if (g_log >= 2)
                    LOGI("damagecopy rect=(%d,%d,%d,%d) bytes=%zu/%zu", q.l, q.t, q.r, q.b,
                         bytes, n);
                return dst;
            }
        }
    }
    bool xproc_job = false;
    if (n == 4147200 && g_xproc_vendor && t_job.active && t_job.matched &&
        !xproc_circuit_open() && xproc_mode_is_fast()) {
        if (!wait_fence(t_job.snap.fence, g_fast_wait_ms)) {
            if (g_log >= 1 && (g_st_memcpy.n <= 4 || (g_st_memcpy.n % 60) == 0))
                LOGW("xproc staging copy not skipped: job fence not signalled");
        } else {
            xproc_job = true;
            t_job.fence_waited = true;
        }
    }
    const bool skip_ok = xproc_job || (g_skipcopy && !g_xproc_vendor);
    if (skip_ok && n == 4147200) {
        // The vendor copies the whole RGBA client target into a staging buffer
        // and then overwrites that buffer with the converted gray. Skip only the
        // exact redundant copy; everything else is untouched. Record that this
        // particular job has no valid staging copy so a helper failure uses the
        // bound client target directly instead of reading stale memory.
        if (xproc_job) t_job.copy_skipped = true;
        __atomic_add_fetch(&g_skipcopy_hits, 1, __ATOMIC_RELAXED);
        return dst;
    }
    void* r = real_memcpy(dst, src, n);
    uint64_t us = (now_ns() - t0) / 1000;
    if (n >= 1048576) {
        stat_add(&g_st_memcpy, us);
        if (g_log >= 1 && (g_st_memcpy.n % 20) == 0)
            LOGI("memcpy %lluus n=%zu dst=%p src=%p (calls=%llu avg=%lluus)",
                 (unsigned long long)us, n, dst, src, (unsigned long long)g_st_memcpy.n,
                 (unsigned long long)stat_avg(&g_st_memcpy));
    }
    return r;
}

static int64_t hook_process_image(void* self, void* param, void* ci) {
    // Once the helper path has committed successfully, the vendor's asynchronous
    // worker has no output to consume. Skip its full 4 MB staging copy as well as
    // its conversion; otherwise it would compete with SF/helper on the same CPU.
    if (g_xproc && !g_xproc_vendor &&
            __atomic_load_n(&g_fast_active, __ATOMIC_RELAXED)) return 0;
    job_begin(self, param, ci);
    // CommitInfo+0x54 is the pixel format the whole e-ink pipeline uses
    // (ComposerHandler::process copies it from the job; triggerEinkCommit puts it
    // in the ioctl; processImage and convertLayerToGray branch on it). Zero means
    // "gray8, convert on the CPU"; non-zero means "the caller's buffer is RGBA,
    // let the kernel convert". Writing it here is the whole 32 ms fix: the
    // vendor's convertToGray is then skipped by its own code path.
    if ((g_skipconv || g_fastgray) && g_buildid_ok && ci) {
        uint32_t* fmt = (uint32_t*)((uint8_t*)ci + 0x54);
        uint32_t old = *fmt;
        if (old == 0) {
            *fmt = (uint32_t)(g_skipconv_fmt > 0 ? g_skipconv_fmt : 1);
            if (g_log >= 1 && (g_st_pi.n <= 4))
                LOGI("skipconv: CommitInfo format 0 -> 0x%x (vendor conversion skipped)", *fmt);
        }
    }
    uint64_t t0 = now_ns();
    t_pi_start = t0;
    int64_t r = real_process_image ? real_process_image(self, param, ci) : 0;
    uint64_t us = (now_ns() - t0) / 1000;
    stat_add(&g_st_pi, us);
    if (g_log >= 1 && (g_st_pi.n % 20) == 0)
        LOGI("processImage %lluus (n=%llu avg=%lluus max=%lluus)",
             (unsigned long long)us, (unsigned long long)g_st_pi.n,
             (unsigned long long)stat_avg(&g_st_pi), (unsigned long long)g_st_pi.max_us);
    job_end();
    return r;
}

static int64_t hook_convert_layer_gray(void* self, void* param, void* ci) {
    if (g_xproc && !g_xproc_vendor &&
            __atomic_load_n(&g_fast_active, __ATOMIC_RELAXED)) return 0;
    job_begin(self, param, ci);
    uint64_t t0 = now_ns();
    int64_t r = real_convert_layer_gray ? real_convert_layer_gray(self, param, ci) : 0;
    uint64_t us = (now_ns() - t0) / 1000;
    stat_add(&g_st_clg, us);
    if (g_log >= 1 && (g_st_clg.n % 20) == 0)
        LOGI("convertLayerToGray %lluus (n=%llu avg=%lluus max=%lluus)",
             (unsigned long long)us, (unsigned long long)g_st_clg.n,
             (unsigned long long)stat_avg(&g_st_clg), (unsigned long long)g_st_clg.max_us);
    job_end();
    return r;
}

// xrz::EffectHandler::convertToGray(src, dst, rect, stride, bpp) -- the single
// function that turns the RGBA client target into the gray8 buffer the panel
// gets, called by both processImage (CT -> commit buffer) and
// convertLayerToGray (in place). It is a static member, PLT-called, so it can be
// observed and (later) replaced without touching the vendor's pipeline.
struct AndRect { int32_t l, t, r, b; };
typedef void (*PFN_convertToGray)(void*, void*, AndRect, int, int);
static PFN_convertToGray real_convert_to_gray;
static uint64_t g_conv_calls;
static uint8_t* g_verify_buf;
static size_t   g_verify_len;

static void hook_convert_to_gray(void* src, void* dst, AndRect rect, int stride, int bpp) {
    ++g_conv_calls;
    int w = rect.r - rect.l, h = rect.b - rect.t;
    if (g_log >= 1 && g_conv_calls <= 8)
        LOGI("convertToGray #%llu src=%p dst=%p rect=(%d,%d,%d,%d) stride=%d bpp=%d inplace=%d fastconv=%d ctsrc=%d",
             (unsigned long long)g_conv_calls, src, dst, rect.l, rect.t, rect.r, rect.b,
             stride, bpp, src == dst, g_fastconv, g_ctsrc);

    if (g_xproc_vendor && t_job.active && (t_job.helper_done || t_job.helper_ambiguous)) return;
    if (g_xproc_vendor && src != dst && w > 0 && h > 0 && stride >= w * 4) {
        const bool fast_mode = xproc_mode_is_fast();
        const int dst_pitch = g_conv_dst_pitch > 0 ? g_conv_dst_pitch : stride / 4;
        uint8_t* dst_ptr = (uint8_t*)dst + (size_t)rect.t * dst_pitch + (size_t)rect.l;
        if (!t_job.active) {
            if (g_log >= 1 && g_st_gray.n <= 4)
                LOGW("xproc vendor conversion skipped: no active job binding");
        } else if (!t_job.matched) {
            if (g_log >= 1 && g_st_gray.n <= 4)
                LOGW("xproc vendor conversion skipped: job client target has no snapshot");
        } else if (t_job.snap.w != w || t_job.snap.h != h || !t_job.snap.handle) {
            if (g_log >= 1 && g_st_gray.n <= 4)
                LOGW("xproc vendor conversion skipped: geometry mismatch (%dx%d vs %dx%d)",
                     t_job.snap.w, t_job.snap.h, w, h);
        } else if (fast_mode && t_job.dst_fd >= 0 &&
                   !dest_quarantined_fd(t_job.dst_fd)) {
            if (!wait_fence(t_job.snap.fence, g_fast_wait_ms)) {
                if (g_log >= 1 && (g_st_gray.n <= 4 || (g_xproc_fails % 20) == 0))
                    LOGW("xproc job fence not signalled in %dms; vendor converter stays in charge",
                         g_fast_wait_ms);
            } else {
                t_job.fence_waited = true;
                const uint32_t vm = __atomic_load_n(&g_vendor_mode, __ATOMIC_RELAXED);
                const uint32_t mode = g_mode >= 0 ? (uint32_t)g_mode
                                      : (g_global_mode >= 0 ? (uint32_t)g_global_mode
                                         : (vm != 0xffffffff ? vm : 0x11));
                int out_pitch = 0;
                bool ambiguous = false;
                const uint64_t t0 = now_ns();
                int fd = xproc_convert(t_job.snap, mode, &out_pitch, true, nullptr,
                                       t_job.dst_fd, &ambiguous,
                                       t_job.damage_valid ? Rect{ t_job.damage_l, t_job.damage_t,
                                                                   t_job.damage_r, t_job.damage_b }
                                                          : Rect{ 0, 0, -1, -1 });
                if (fd >= 0) {
                    close(fd);
                    t_job.helper_done = true;
                    __atomic_store_n(&g_xproc_ready, 1, __ATOMIC_RELAXED);
                    const uint64_t us = (now_ns() - t0) / 1000;
                    stat_add(&g_st_gray, us);
                    if (g_log >= 1 && (g_st_gray.n <= 4 || (g_st_gray.n % 20) == 0))
                        LOGI("xproc vendor convert+raw-copy %lluus (n=%llu avg=%lluus max=%lluus)",
                             (unsigned long long)us, (unsigned long long)g_st_gray.n,
                             (unsigned long long)stat_avg(&g_st_gray),
                             (unsigned long long)g_st_gray.max_us);
                    return;
                }
                if (g_log >= 1)
                    LOGW("xproc vendor conversion failed%s; restoring vendor converter",
                         ambiguous ? " (destination quarantined)" : "");
                if (ambiguous) {
                    // The helper may still be writing this destination. Do not
                    // race it with the CPU fallback; drop this commit instead.
                    pthread_mutex_lock(&g_lock);
                    g_drop_next_commit = true;
                    t_job.helper_ambiguous = true;
                    pthread_mutex_unlock(&g_lock);
                    if (g_log >= 1)
                        LOGE("xproc request outcome ambiguous; dropping next commit");
                    return;
                }
            }
        }
        // If the staging copy was skipped, convert the same job's snapshot
        // directly into the vendor's locked destination buffer (`dst`).
        if (t_job.copy_skipped) {
            if (job_direct_fallback(dst_ptr, w, h, dst_pitch)) {
                t_job.helper_done = true;
                __atomic_add_fetch(&g_xproc_fails, 1, __ATOMIC_RELAXED);
                if (g_log >= 1)
                    LOGW("xproc failed; used same-frame CPU fallback from bound snapshot");
                return;
            }
            pthread_mutex_lock(&g_lock);
            g_drop_next_commit = true;
            pthread_mutex_unlock(&g_lock);
            if (g_log >= 1)
                LOGE("xproc fallback unavailable; dropping next commit");
            return;
        }
    }

    if (!g_fastconv || !src || !dst || w <= 0 || h <= 0 || stride < w * 4) {
        if (real_convert_to_gray) real_convert_to_gray(src, dst, rect, stride, bpp);
        return;
    }

    // With skipcopy the vendor's staging buffer is never filled, so read the
    // client target directly. The geometry must be the client target's own.
    const uint8_t* sp = (const uint8_t*)src + (size_t)rect.t * stride + (size_t)rect.l * 4;
    int spitch = stride;
    bool mapped = false;
    bool mapper_locked = false;
    const native_handle_t* imp_used = nullptr;
    SnapLease snap_lease;
    CtSnap& snap = snap_lease.snap;
    if (g_skipcopy && g_ctsrc) {
        pthread_mutex_lock(&g_lock);
        ct_snap_clone_locked(g_snap, &snap);
        pthread_mutex_unlock(&g_lock);
        if (snap.valid && snap.w == w && snap.h == h) {
            // The mapper import + lock gives the cached mapping (2.2 ms / 4 MB).
            // Raw mmap of the same dma-buf is ~80 ms, so only fall back to it if
            // the mapper is unavailable.
            if (load_libui()) {
                pthread_mutex_lock(&g_lock);
                imp_used = g_ct_imp;
                pthread_mutex_unlock(&g_lock);
            }
            if (imp_used) {
                if (!g_gbm) g_gbm = p_gbm_instance ? p_gbm_instance() : nullptr;
                Rect b = { 0, 0, w, h };
                void* addr = nullptr;
                int32_t bpp = 0, bps = 0;
                if (g_gbm && p_gbm_lock(g_gbm, imp_used, GRALLOC_USAGE_SW_READ_RARELY, &b,
                                        &addr, &bpp, &bps) == 0 && addr) {
                    sp = (const uint8_t*)addr;
                    spitch = bps > 0 ? bps : w * 4;
                    mapped = true;
                    mapper_locked = true;
                }
            }
            if (!mapped) {
                uint8_t* base = nullptr; size_t len = 0;
                if (ct_map(snap, false, &base, &len)) {
                    sp = base;
                    spitch = snap.stride * 4;
                    mapped = true;
                }
            }
            if (!mapped) {
                if (g_log >= 1) LOGW("ctsrc: client target not mappable (%dx%d)", snap.w, snap.h);
                if (real_convert_to_gray) real_convert_to_gray(src, dst, rect, stride, bpp);
                return;
            }
        } else {
            if (g_log >= 1) LOGW("ctsrc: no usable client target (%dx%d vs %dx%d)",
                                 snap.w, snap.h, w, h);
            if (real_convert_to_gray) real_convert_to_gray(src, dst, rect, stride, bpp);
            return;
        }
    }

    int dp = g_conv_dst_pitch > 0 ? g_conv_dst_pitch : stride / 4;
    uint8_t* d = (uint8_t*)dst + (size_t)rect.t * dp + (size_t)rect.l;
    // 1-bit modes: the kernel does not dither A2/Hyper/Extreme, so a mid-grey
    // would be a partial drive. Dither to pure black/white instead.
    uint32_t vlo = __atomic_load_n(&g_vendor_mode, __ATOMIC_RELAXED) & 0xff;
    bool onebit = g_dither1 &&
                  (vlo == 0x10 || vlo == 0x11 || vlo == 0xb3 || vlo == 0xb7);

    auto release_source = [&]() {
        if (mapper_locked && imp_used) {
            if (g_gbm && p_gbm_unlock) p_gbm_unlock(g_gbm, imp_used);
        } else if (mapped) {
            ct_unmap_sync(snap, false);
        }
        mapped = false;
        mapper_locked = false;
    };

    if (g_verify && src != dst) {
        if (!real_convert_to_gray) { release_source(); return; }
        real_convert_to_gray(src, dst, rect, stride, bpp);      // the reference
        size_t need = (size_t)dp * (size_t)h;
        if (!g_verify_buf || g_verify_len < need) {
            free(g_verify_buf);
            g_verify_buf = (uint8_t*)malloc(need);
            g_verify_len = g_verify_buf ? need : 0;
        }
        if (!g_verify_buf) { release_source(); return; }
        memcpy(g_verify_buf, dst, need);
        memset(dst, 0, need);                                    // make mistakes visible
        gray_conv((uint8_t*)dst, (uint8_t*)src, w, h, stride, dp, false);
        uint64_t sum = 0, n = 0; int mx = 0, bad = 0;
        for (int y = 0; y < h; y++) {
            const uint8_t* a = g_verify_buf + (size_t)y * dp;
            const uint8_t* b = (uint8_t*)dst + (size_t)y * dp;
            for (int x = 0; x < w; x++) {
                int diff = (int)a[x] - (int)b[x];
                if (diff < 0) diff = -diff;
                sum += (uint64_t)diff; n++;
                if (diff > mx) mx = diff;
                if (diff > 32) bad++;
            }
        }
        if (g_log >= 1 && (g_conv_calls % 20) == 0)
            LOGI("verify: pitch=%d %dx%d srcpitch=%d mean=%.2f max=%d bad>32=%llu/%llu",
                 dp, w, h, stride, n ? (double)sum / (double)n : 0.0, mx,
                 (unsigned long long)bad, (unsigned long long)n);
        memcpy(dst, g_verify_buf, need);                          // keep the vendor's output
        release_source();
        return;
    }

    uint64_t t0 = now_ns();
    gray_conv(d, sp, w, h, spitch, dp, onebit);
    uint64_t us = (now_ns() - t0) / 1000;
    release_source();
    stat_add(&g_st_conv, us);
    if (g_log >= 1 && (g_st_conv.n % 40) == 0)
        LOGI("fastconv %lluus (n=%llu avg=%lluus max=%lluus) %dx%d pitch=%d/%d dst=%p src=%p",
             (unsigned long long)us, (unsigned long long)g_st_conv.n,
             (unsigned long long)stat_avg(&g_st_conv), (unsigned long long)g_st_conv.max_us,
             w, h, dp, stride, dst, src);
}

// ---------------------------------------------------------------- vsync hook

// mtk_waitVSync(int64_t* timestamp): ioctl _IOR(0x4f,0xd5,24) on the e-ink fd.
// If the composer's commit loop paces itself on this, no-opping it removes the
// pacing; if the interval does not change, the panel itself is the floor.
static int hook_mtk_waitVSync(int64_t* ts) {
    maybe_reload_knobs();
    uint64_t t0 = now_ns();
    int ret;
    if (g_vend_hz > 0) {
        // Fast mode owns the commit path. Stretch the vendor worker's pacing so it
        // spends its ~32 ms conversion only a couple of times a second instead of
        // pinning a core next to SurfaceFlinger's own composition work.
        uint64_t period = 1000000000ull / (uint64_t)g_vend_hz;
        uint64_t now = t0;
        if (g_last_vend_ns && now - g_last_vend_ns < period) {
            struct timespec req = { 0, (long)(period - (now - g_last_vend_ns)) };
            nanosleep(&req, nullptr);
        }
        g_last_vend_ns = now_ns();
        if (ts) *ts = (int64_t)g_last_vend_ns;
        ret = 0;
    } else if (g_novsync) {
        if (ts) *ts = (int64_t)t0;
        ret = 0;
    } else {
        ret = real_mtk_waitvsync(ts);
    }
    uint64_t us = (now_ns() - t0) / 1000;
    if (g_log >= 1) {
        __atomic_add_fetch(&g_vsync_calls, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_vsync_us_total, us, __ATOMIC_RELAXED);
    }
    return ret;
}

// ---------------------------------------------------------------- GOT hooking

struct HookSpec { const char* sym; void* hook; void** orig; int hits; };

static int got_cb(struct dl_phdr_info* info, size_t, void* data) {
    HookSpec* spec = (HookSpec*)data;
    if (!info->dlpi_name || !strstr(info->dlpi_name, REAL_HWC_MATCH)) return 0;
    ElfW(Addr) bias = info->dlpi_addr;
    const ElfW(Dyn)* dyn = nullptr;
    for (int i = 0; i < info->dlpi_phnum; i++)
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC)
            dyn = (const ElfW(Dyn)*)(bias + info->dlpi_phdr[i].p_vaddr);
    if (!dyn) { LOGE("got: no PT_DYNAMIC"); return 1; }

    const ElfW(Sym)* symtab = nullptr;
    const char* strtab = nullptr;
    const ElfW(Rela)* jmprel = nullptr;
    size_t jmprelsz = 0;
    const ElfW(Rela)* rela = nullptr;
    size_t relasz = 0;
    for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB: symtab = (const ElfW(Sym)*)(bias + d->d_un.d_ptr); break;
            case DT_STRTAB: strtab = (const char*)(bias + d->d_un.d_ptr); break;
            case DT_JMPREL: jmprel = (const ElfW(Rela)*)(bias + d->d_un.d_ptr); break;
            case DT_PLTRELSZ: jmprelsz = d->d_un.d_val; break;
            case DT_RELA: rela = (const ElfW(Rela)*)(bias + d->d_un.d_ptr); break;
            case DT_RELASZ: relasz = d->d_un.d_val; break;
        }
    }
    if (!symtab || !strtab) { LOGE("got: no symtab/strtab"); return 1; }

    auto scan = [&](const ElfW(Rela)* r, size_t sz) {
        if (!r) return;
        for (size_t i = 0; i < sz / sizeof(ElfW(Rela)); i++) {
            uint32_t type = ELF64_R_TYPE(r[i].r_info);
            if (type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_GLOB_DAT) continue;
            const char* name = strtab + symtab[ELF64_R_SYM(r[i].r_info)].st_name;
            if (strcmp(name, spec->sym) != 0) continue;
            void** slot = (void**)(bias + r[i].r_offset);
            uintptr_t page = (uintptr_t)slot & ~(uintptr_t)(getpagesize() - 1);
            if (mprotect((void*)page, getpagesize(), PROT_READ | PROT_WRITE) != 0) {
                LOGE("got: mprotect %p: %s", slot, strerror(errno));
                continue;
            }
            if (!*spec->orig) *spec->orig = *slot;
            *slot = spec->hook;
            spec->hits++;
        }
    };
    scan(jmprel, jmprelsz);
    scan(rela, relasz);
    return 0;   // keep going: the shim itself also matches REAL_HWC_MATCH in the
                // "replace" layout but has no mtk_commit relocation
}

static void install_out_hooks() {
    // C++ mangled names of the exported functions the commit path uses.
    static HookSpec commit_spec = { "_Z10mtk_commit18eink_update_config",
                                    (void*)hook_mtk_commit, (void**)&real_mtk_commit, 0 };
    static HookSpec vsync_spec = { "_Z13mtk_waitVSyncPl",
                                   (void*)hook_mtk_waitVSync, (void**)&real_mtk_waitvsync, 0 };
    static HookSpec pi_spec = { "_ZN13OverlayEngine12processImageEP16OverlayPortParamP10CommitInfo",
                                (void*)hook_process_image, (void**)&real_process_image, 0 };
    static HookSpec clg_spec = { "_ZN13OverlayEngine18convertLayerToGrayEP16OverlayPortParamP10CommitInfo",
                                 (void*)hook_convert_layer_gray, (void**)&real_convert_layer_gray, 0 };
    static HookSpec conv_spec = { "_ZN3xrz13EffectHandler13convertToGrayEPvS1_N7android4RectEii",
                                  (void*)hook_convert_to_gray, (void**)&real_convert_to_gray, 0 };
    static HookSpec memcmp_spec = { "memcmp", (void*)hook_memcmp, (void**)&real_memcmp, 0 };
    static HookSpec sync_spec = { "sync_wait", (void*)hook_sync_wait, (void**)&real_sync_wait, 0 };
    static HookSpec gblock_spec = { "_ZN7android13GraphicBuffer4lockEjPPvPiS3_",
                                    (void*)hook_gb_lock, (void**)&real_gb_lock, 0 };
    static HookSpec memcpy_spec = { "memcpy", (void*)hook_memcpy, (void**)&real_memcpy, 0 };
    dl_iterate_phdr(got_cb, &commit_spec);
    dl_iterate_phdr(got_cb, &vsync_spec);
    // processImage is always hooked: it is where skipconv writes CommitInfo+0x54.
    // convertLayerToGray is timing-only (probe=1).
    dl_iterate_phdr(got_cb, &pi_spec);
    dl_iterate_phdr(got_cb, &conv_spec);
    dl_iterate_phdr(got_cb, &memcmp_spec);
    dl_iterate_phdr(got_cb, &sync_spec);
    dl_iterate_phdr(got_cb, &gblock_spec);
    dl_iterate_phdr(got_cb, &memcpy_spec);
    if (g_probe) dl_iterate_phdr(got_cb, &clg_spec);
    if (commit_spec.hits == 0) LOGE("mtk_commit GOT slot not found -- shim is passive");
    else LOGI("mtk_commit hooked (%d slot%s), real=%p", commit_spec.hits,
              commit_spec.hits == 1 ? "" : "s", (void*)real_mtk_commit);
    LOGI("mtk_waitVSync %s (%d), processImage %s (%d), convertLayerToGray %s (%d), convertToGray %s (%d)",
         vsync_spec.hits ? "hooked" : "absent", vsync_spec.hits,
         pi_spec.hits ? "hooked" : "absent", pi_spec.hits,
         clg_spec.hits ? "hooked" : "absent", clg_spec.hits,
         conv_spec.hits ? "hooked" : "absent", conv_spec.hits);
}

// ---------------------------------------------------------------- getFunction

static hwc2_function_pointer_t shim_getFunction(hwc2_device_t* dev, int32_t desc) {
    hwc2_function_pointer_t real = g_real_getFunction(dev, desc);
    if (desc > 0 && desc < FN_MAX_PROBE) g_real_fn[desc] = real;
    if (!real) return real;
    switch (desc) {
        case FN_CREATE_LAYER:               return (hwc2_function_pointer_t)hook_createLayer;
        case FN_DESTROY_LAYER:              return (hwc2_function_pointer_t)hook_destroyLayer;
        case FN_DUMP:                       return (hwc2_function_pointer_t)hook_dump;
        case FN_PRESENT_DISPLAY:            return (hwc2_function_pointer_t)hook_presentDisplay;
        case FN_SET_CLIENT_TARGET:          return (hwc2_function_pointer_t)hook_setClientTarget;
        case FN_SET_LAYER_COMPOSITION_TYPE: return (hwc2_function_pointer_t)hook_setLayerCompositionType;
        case FN_SET_LAYER_DISPLAY_FRAME:    return (hwc2_function_pointer_t)hook_setLayerDisplayFrame;
        case FN_SET_LAYER_SURFACE_DAMAGE:   return (hwc2_function_pointer_t)hook_setLayerSurfaceDamage;
        case FN_SET_LAYER_TRANSFORM:        return (hwc2_function_pointer_t)hook_setLayerTransform;
        case FN_SET_POWER_MODE:             return (hwc2_function_pointer_t)hook_setPowerMode;
        default:                            return real;
    }
}

static void probe_functions(hwc2_device_t* dev) {
    char line[512];
    int n = snprintf(line, sizeof(line), "real HWC descriptors:");
    for (int d = 1; d < 64; d++) {
        hwc2_function_pointer_t f = g_real_getFunction(dev, d);
        g_real_fn[d] = f;
        if (f) n += snprintf(line + n, sizeof(line) - n, " %d", d);
    }
    LOGI("%s", line);
    LOGI("real HWC: mtk_commit export=%p  setClientTarget=%p  surfaceDamage=%p",
         dlsym(g_real_handle, "_Z10mtk_commit18eink_update_config"),
         (void*)g_real_fn[FN_SET_CLIENT_TARGET],
         (void*)g_real_fn[FN_SET_LAYER_SURFACE_DAMAGE]);
}

// ---------------------------------------------------------------- module entry

// The offset used by skipconv (CommitInfo+0x54) is BuildID-specific, exactly the
// class of peek the B1051C review warns about. Refuse it on any other build.
static const uint8_t KNOWN_BUILD_ID[16] = {
    0x0b, 0xc3, 0xc3, 0x5a, 0x2f, 0xfd, 0x85, 0xff,
    0x39, 0xc7, 0x68, 0xde, 0xb4, 0xb0, 0x23, 0x0b,
};

static bool buildid_matches(const char* path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    uint8_t eh[sizeof(ElfW(Ehdr))];
    bool ok = false;
    if (pread(fd, eh, sizeof eh, 0) == (ssize_t)sizeof eh && !memcmp(eh, ELFMAG, SELFMAG)) {
        const ElfW(Ehdr)* e = (const ElfW(Ehdr)*)eh;
        if (e->e_phentsize == sizeof(ElfW(Phdr))) {
            for (int i = 0; i < (int)e->e_phnum && !ok; i++) {
                ElfW(Phdr) ph;
                if (pread(fd, &ph, sizeof ph, (off_t)(e->e_phoff + (ElfW(Off))i * sizeof ph)) !=
                    (ssize_t)sizeof ph) continue;
                if (ph.p_type != PT_NOTE || ph.p_filesz < 12 || ph.p_filesz > (1u << 20)) continue;
                uint8_t* buf = (uint8_t*)malloc(ph.p_filesz);
                if (!buf) continue;
                if (pread(fd, buf, ph.p_filesz, (off_t)ph.p_offset) == (ssize_t)ph.p_filesz) {
                    size_t off = 0;
                    while (off + 12 <= ph.p_filesz) {
                        uint32_t namesz, descsz, type;
                        memcpy(&namesz, buf + off, 4);
                        memcpy(&descsz, buf + off + 4, 4);
                        memcpy(&type, buf + off + 8, 4);
                        if (namesz == 0 && descsz == 0) break;
                        size_t desc = off + 12 + ((namesz + 3u) & ~3u);
                        if (type == NT_GNU_BUILD_ID && namesz == 4 && descsz == 16 &&
                            desc + 16 <= ph.p_filesz && !memcmp(buf + off + 12, "GNU", 4) &&
                            !memcmp(buf + desc, KNOWN_BUILD_ID, 16)) ok = true;
                        off = desc + ((descsz + 3u) & ~3u);
                    }
                }
                free(buf);
            }
        }
    }
    close(fd);
    return ok;
}

static int shim_open(const hw_module_t* /*module*/, const char* name, hw_device_t** device) {
    __android_log_print(ANDROID_LOG_INFO, TAG, "shim_open entered (name=%s)", name ? name : "?");
    read_knobs();
    int disabled = prop_int("vendor.debug.hwc.shim.disable", 0);
    LOGI("hwcshim mt6765: log=%d rect=%d mode=%d src=%d xform=%d disabled=%d fast=%d inplace=%d diff=%d vend_hz=%d skipconv=%d fastgray=%d damagecopy=%d",
         g_log, g_rect, g_mode, g_src, g_xform, disabled, g_fast, g_inplace, g_fast_diff, g_vend_hz, g_skipconv, g_fastgray, g_damagecopy);

    // dlopen the real composer. Reject our own module: in the "replace" layout the
    // original path now points at the shim, and dlopening it would recurse.
    Dl_info self_info;
    bool have_self = dladdr((void*)&HMI, &self_info) != 0 && self_info.dli_fname;
    for (size_t i = 0; i < sizeof(REAL_HWC_CANDIDATES) / sizeof(REAL_HWC_CANDIDATES[0]); i++) {
        const char* cand = REAL_HWC_CANDIDATES[i];
        if (access(cand, R_OK) != 0) {
            LOGI("real candidate %s: not present", cand);
            continue;
        }
        if (have_self) {
            struct stat a = {}, b = {};
            if (stat(cand, &b) == 0 && stat(self_info.dli_fname, &a) == 0 &&
                a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
                LOGI("real candidate %s: is this shim, skipping", cand);
                continue;
            }
        }
        g_real_handle = dlopen(cand, RTLD_NOW | RTLD_LOCAL);
        if (g_real_handle) {
            REAL_HWC = cand;
            g_buildid_ok = buildid_matches(cand);
            LOGI("real HWC loaded from %s (BuildID %s)", cand,
                 g_buildid_ok ? "matches 0bc3c35a2ffd85ff39c768deb4b0230b" : "DIFFERS");
            if (g_skipconv && !g_buildid_ok)
                LOGE("skipconv disabled: CommitInfo+0x54 is a BuildID-specific offset");
            break;
        }
        LOGE("dlopen(%s) failed: %s", cand, dlerror());
    }
    if (!g_real_handle) {
        LOGE("no real HWC could be loaded -- refusing to open (composer stays down)");
        return -ENOENT;
    }
    g_real_module = (hw_module_t*)dlsym(g_real_handle, "HMI");    if (!g_real_module) {
        LOGE("real HMI not found: %s", dlerror());
        return -EINVAL;
    }
    LOGI("real module id=%s name=%s author=%s", g_real_module->id,
         g_real_module->name ? g_real_module->name : "?",
         g_real_module->author ? g_real_module->author : "?");

    // gralloc_extra_query is how MTK exposes buffer geometry/ion fd. The real HWC
    // is dlopened RTLD_LOCAL, so open its dependency explicitly.
    void* ge = dlopen("libgralloc_extra.so", RTLD_NOW | RTLD_LOCAL);
    if (ge) {
        *(void**)&real_gralloc_extra_query = dlsym(ge, "gralloc_extra_query");
        if (!real_gralloc_extra_query) {
            // fall back to the prefixed name some builds use
            *(void**)&real_gralloc_extra_query = dlsym(ge, "_Z20gralloc_extra_queryP13native_handlejPv");
        }
    }
    LOGI("gralloc_extra_query: %p", (void*)real_gralloc_extra_query);

    hw_device_t* dev = nullptr;
    int err = g_real_module->methods->open(g_real_module, name, &dev);
    if (err != 0 || !dev) {
        LOGE("real open failed: %d", err);
        return err ? err : -EINVAL;
    }
    g_dev = (hwc2_device_t*)dev;
    LOGI("real device version=0x%x (major %u)", dev->version, (dev->version >> 24) & 0xf);

    if (!disabled) {
        g_real_getFunction = g_dev->getFunction;
        g_dev->getFunction = shim_getFunction;
        probe_functions(g_dev);
        install_out_hooks();
    } else {
        LOGI("disabled: pure passthrough");
    }
    *device = dev;
    return 0;
}

static hw_module_methods_t shim_methods = { shim_open };

__attribute__((constructor)) static void shim_ctor(void) {
    __android_log_print(ANDROID_LOG_INFO, TAG, "module dlopened (constructor)");
}

extern "C" __attribute__((visibility("default"))) hw_module_t HMI = {
    HARDWARE_MODULE_TAG,
    /*module_api_version*/ 0x0300,
    /*hal_api_version*/    0x0100,
    "hwcomposer",
    "hwcshim interposer for hwcomposer.mt6765.so",
    "hibreak",
    &shim_methods,
    nullptr,
    {0},
};
