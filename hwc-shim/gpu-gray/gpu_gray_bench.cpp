// gpu_gray_bench — can the GPU, inside the composer, turn the 4 MB RGBA client
// target into gray8 faster than the vendor pipeline's 4 MB staging copy?
//
// Background (FINDING-fast-commit.md, NOTES.md 2026-11-24 06:50): the ceiling on
// this GSI is the composer reading the 4 MB RGBA client target. The buffer is a
// gralloc dma-buf whose CPU mapping is uncached on this kernel, so a
// CPU RGBA->gray8 pass costs 31-80 ms (avg 33.5 ms) and caps the panel at
// ~28-30 fps. The GPU does not care about CPU cacheability, so the same read
// done by the GPU should cost a few ms.
//
// This program measures, on the phone, with the same allocator the compositor
// uses:
//
//   [1] cpu  RGBA->gray8  malloc dst        the read cost alone
//   [2] cpu  RGBA->gray8  gralloc R8 dst    read + uncached 1 MB write
//   [3] cpu  memcpy 4 MB  malloc dst        raw read bandwidth, for reference
//   [4] gpu  RGBA->gray8  gralloc R8 dst    the candidate shim path
//   [5] gpu  RGBA->gray8  gralloc RGBA dst  same but a 4 MB target
//
// and verifies [4]/[5] against the CPU converter (the shim's own maths, so a
// match means the shader is exact, not merely close).
//
// It is a standalone benchmark: it does not touch SurfaceFlinger and does not
// load the HWC shim. Nothing is written outside /data/local/tmp.
//
// Build: build.sh (Linux/macOS) or build.ps1 (Windows). Run: run.sh.

#define LOG_TAG "gpu-gray"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <arm_neon.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ---------------------------------------------------------------------------
// knobs

static int g_w = 1440;          // the e-ink controller space, not the 720x1440
static int g_h = 720;           // display space: this is what the vendor copies
static int g_iters = 20;
static int g_warmup = 3;
static int g_cpu_iters = 5;
static int g_verify = 1;
static int g_src_cached = 0;    // allocate the source with CPU_*_OFTEN

// ---------------------------------------------------------------------------
// timing

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int cmp_double(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

// Report min / median / mean of `n` samples. `mb` > 0 adds an MB/s figure for
// the fastest sample.
static void report(const char* tag, double* v, int n, double mb) {
    qsort(v, (size_t)n, sizeof(double), cmp_double);
    double sum = 0;
    for (int i = 0; i < n; i++) sum += v[i];
    if (mb > 0) {
        printf("%-28s min %7.2f  med %7.2f  mean %7.2f ms   (%5.1f MB/s)\n", tag, v[0],
               v[n / 2], sum / n, mb / (v[0] / 1000.0));
    } else {
        printf("%-28s min %7.2f  med %7.2f  mean %7.2f ms\n", tag, v[0], v[n / 2], sum / n);
    }
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// the CPU converter, copied from hwcshim_mt6765.cpp (gray_conv): BT.601-ish
// 77/150/29, ordered 4x4 dither, 16 levels, no division. This is both the
// baseline and the reference the GPU output is checked against.

static const uint8_t kBayer16[4][16] = {
    {  0,  8,  2, 10,  0,  8,  2, 10,  0,  8,  2, 10,  0,  8,  2, 10 },
    { 12,  4, 14,  6, 12,  4, 14,  6, 12,  4, 14,  6, 12,  4, 14,  6 },
    {  3, 11,  1,  9,  3, 11,  1,  9,  3, 11,  1,  9,  3, 11,  1,  9 },
    { 15,  7, 13,  5, 15,  7, 13,  5, 15,  7, 13,  5, 15,  7, 13,  5 },
};

static void gray_conv(uint8_t* dst, const uint8_t* src, int w, int h, int src_pitch, int dst_pitch) {
    for (int y = 0; y < h; y++) {
        const uint8_t* s = src + (size_t)y * src_pitch;
        uint8_t* d = dst + (size_t)y * dst_pitch;
        uint8x16_t bv = vld1q_u8(kBayer16[y & 3]);
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
            unsigned l = ((unsigned)px[0] * 77u + (unsigned)px[1] * 150u + (unsigned)px[2] * 29u) >> 8;
            unsigned q = (l + kBayer16[y & 3][x & 15]) >> 4;
            if (q > 15) q = 15;
            d[x] = (uint8_t)(q * 17u);
        }
    }
}

// ---------------------------------------------------------------------------
// AHardwareBuffer helpers

static int alloc_buf(AHardwareBuffer** out, uint32_t format, uint64_t usage, const char* what) {
    AHardwareBuffer_Desc d;
    memset(&d, 0, sizeof(d));
    d.width = (uint32_t)g_w;
    d.height = (uint32_t)g_h;
    d.layers = 1;
    d.format = format;
    d.usage = usage;
    int r = AHardwareBuffer_allocate(&d, out);
    if (r != 0) {
        printf("  %-10s alloc failed: %d (%s) usage=0x%llx\n", what, r, strerror(r > 0 ? r : -r),
               (unsigned long long)usage);
        *out = NULL;
        return r;
    }
    AHardwareBuffer_Desc got;
    AHardwareBuffer_describe(*out, &got);
    printf("  %-10s fmt=0x%-4x %ux%u stride=%u usage=0x%llx\n", what, got.format, got.width,
           got.height, got.stride, (unsigned long long)got.usage);
    return 0;
}

// ---------------------------------------------------------------------------
// EGL

static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static EGLSurface g_surf = EGL_NO_SURFACE;

static PFNEGLCREATEIMAGEKHRPROC p_eglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImageKHR;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTargetTexture2DOES;
static PFNEGLCREATESYNCKHRPROC p_eglCreateSyncKHR;
static PFNEGLDESTROYSYNCKHRPROC p_eglDestroySyncKHR;
static PFNEGLCLIENTWAITSYNCKHRPROC p_eglClientWaitSyncKHR;
typedef EGLClientBuffer (*PFN_get_native_client_buffer)(const void*);
static PFN_get_native_client_buffer p_eglGetNativeClientBufferANDROID;

static int egl_setup(void) {
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_dpy == EGL_NO_DISPLAY) { printf("eglGetDisplay failed\n"); return -1; }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(g_dpy, &major, &minor)) { printf("eglInitialize failed\n"); return -1; }

    const EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(g_dpy, cfg_attr, &cfg, 1, &n) || n < 1) {
        printf("eglChooseConfig found no ES2 config\n");
        return -1;
    }
    const EGLint pb[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    g_surf = eglCreatePbufferSurface(g_dpy, cfg, pb);
    if (g_surf == EGL_NO_SURFACE) { printf("eglCreatePbufferSurface failed 0x%x\n", eglGetError()); return -1; }

    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (g_ctx == EGL_NO_CONTEXT) { printf("eglCreateContext failed 0x%x\n", eglGetError()); return -1; }
    if (!eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) {
        printf("eglMakeCurrent failed 0x%x\n", eglGetError());
        return -1;
    }

    p_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    p_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    p_glEGLImageTargetTexture2DOES =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    p_eglCreateSyncKHR = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    p_eglDestroySyncKHR = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    p_eglClientWaitSyncKHR = (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");
    p_eglGetNativeClientBufferANDROID =
        (PFN_get_native_client_buffer)eglGetProcAddress("eglGetNativeClientBufferANDROID");

    if (!p_eglCreateImageKHR || !p_glEGLImageTargetTexture2DOES || !p_eglGetNativeClientBufferANDROID) {
        printf("missing EGL image entry points (OES_EGL_image / ANDROID_get_native_client_buffer)\n");
        return -1;
    }
    printf("egl: %d.%d  vendor=%s  version=%s\n", major, minor, eglQueryString(g_dpy, EGL_VENDOR),
           eglQueryString(g_dpy, EGL_VERSION));
    printf("gl : %s | %s\n", (const char*)glGetString(GL_RENDERER), (const char*)glGetString(GL_VERSION));
    return 0;
}

static EGLImageKHR make_image(AHardwareBuffer* b) {
    EGLClientBuffer cb = p_eglGetNativeClientBufferANDROID(b);
    if (!cb) { printf("  eglGetNativeClientBufferANDROID returned NULL\n"); return EGL_NO_IMAGE_KHR; }
    const EGLint attrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
    EGLImageKHR img = p_eglCreateImageKHR(g_dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, attrs);
    if (img == EGL_NO_IMAGE_KHR) printf("  eglCreateImageKHR failed 0x%x\n", eglGetError());
    return img;
}

static GLuint image_texture(EGLImageKHR img) {
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

// ---------------------------------------------------------------------------
// shader: the same maths as gray_conv(), per pixel

static const char* kVert =
    "attribute vec2 aPos;\n"
    "attribute vec2 aUv;\n"
    "varying vec2 vUv;\n"
    "void main() { vUv = aUv; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char* kFrag =
    "precision highp float;\n"
    "varying vec2 vUv;\n"
    "uniform sampler2D uSrc;\n"
    "uniform sampler2D uBayer;\n"
    "uniform vec2 uSize;\n"
    "void main() {\n"
    "    vec4 c = texture2D(uSrc, vUv) * 255.0;\n"
    "    vec2 p = floor(vUv * uSize);\n"
    "    float d = texture2D(uBayer, (p + 0.5) * 0.25).r * 255.0;\n"
    "    float l = (c.r * 77.0 + c.g * 150.0 + c.b * 29.0) * (1.0 / 256.0);\n"
    "    float q = floor((l + d) * (1.0 / 16.0));\n"
    "    q = min(q, 15.0);\n"
    "    float g = q * 17.0 * (1.0 / 255.0);\n"
    "    gl_FragColor = vec4(g, g, g, 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetShaderInfoLog(s, sizeof(log) - 1, NULL, log);
        printf("shader compile failed: %s\n", log);
        return 0;
    }
    return s;
}

static GLuint make_program(void) {
    GLuint vs = compile(GL_VERTEX_SHADER, kVert);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFrag);
    if (!vs || !fs) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "aPos");
    glBindAttribLocation(p, 1, "aUv");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetProgramInfoLog(p, sizeof(log) - 1, NULL, log);
        printf("link failed: %s\n", log);
        return 0;
    }
    return p;
}

static GLuint make_bayer_texture(void) {
    uint8_t tex[16];
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) tex[y * 4 + x] = kBayer16[y][x];
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, 4, 4, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return t;
}

// ---------------------------------------------------------------------------

static void fill_source(AHardwareBuffer* b) {
    void* p = NULL;
    int r = AHardwareBuffer_lock(b, AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY, -1, NULL, &p);
    if (r != 0) { printf("  source lock for write failed: %d\n", r); return; }
    AHardwareBuffer_Desc d;
    AHardwareBuffer_describe(b, &d);
    for (uint32_t y = 0; y < d.height; y++) {
        uint8_t* row = (uint8_t*)p + (size_t)y * d.stride * 4;
        for (uint32_t x = 0; x < d.width; x++) {
            row[x * 4 + 0] = (uint8_t)((x * 7 + y * 3) & 0xff);
            row[x * 4 + 1] = (uint8_t)((x * 3 + y * 11 + 40) & 0xff);
            row[x * 4 + 2] = (uint8_t)((x * 5 + y * 5 + 90) & 0xff);
            row[x * 4 + 3] = 0xff;
        }
    }
    int32_t fence = -1;
    AHardwareBuffer_unlock(b, &fence);
}

// CPU: gray8 into `dst` with pitch `dst_pitch` (0 = w), timed over g_cpu_iters.
static void bench_cpu(const char* tag, AHardwareBuffer* src, uint8_t* dst_mem, int dst_pitch, double* out) {
    double* t = (double*)calloc((size_t)g_cpu_iters, sizeof(double));
    for (int i = 0; i < g_cpu_iters; i++) {
        void* sp = NULL;
        if (AHardwareBuffer_lock(src, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL, &sp) != 0) {
            printf("%s: source lock failed\n", tag);
            free(t);
            return;
        }
        AHardwareBuffer_Desc d;
        AHardwareBuffer_describe(src, &d);
        double t0 = now_ms();
        gray_conv(dst_mem, (const uint8_t*)sp, g_w, g_h, (int)d.stride * 4, dst_pitch ? dst_pitch : g_w);
        t[i] = now_ms() - t0;
        int32_t fence = -1;
        AHardwareBuffer_unlock(src, &fence);
    }
    report(tag, t, g_cpu_iters, (double)g_w * g_h * 4 / 1e6);
    if (out) memcpy(out, t, sizeof(double));
    free(t);
}

// copy 4 MB out of the source mapping into a malloc'd buffer: raw read speed
static void bench_copy(AHardwareBuffer* src, uint8_t* dst_mem) {
    double t[3];
    volatile uint32_t sink = 0;
    for (int i = 0; i < 3; i++) {
        void* sp = NULL;
        if (AHardwareBuffer_lock(src, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL, &sp) != 0) return;
        AHardwareBuffer_Desc d;
        AHardwareBuffer_describe(src, &d);
        size_t n = (size_t)d.stride * 4 * g_h;
        double t0 = now_ms();
        memcpy(dst_mem, sp, n);
        sink ^= dst_mem[n / 2];     // keep the copy alive
        t[i] = now_ms() - t0;
        int32_t fence = -1;
        AHardwareBuffer_unlock(src, &fence);
    }
    (void)sink;
    report("[3] cpu  memcpy 4MB -> malloc", t, 3, (double)g_w * g_h * 4 / 1e6);
}

// GPU: one pass, timed submit + fence wait. Returns 0 on success.
static double* bench_gpu(const char* tag, GLuint prog, GLuint src_tex, GLuint bayer_tex,
                         GLuint dst_tex, int dst_rgba) {
    int n = g_iters;
    double* t = (double*)calloc((size_t)n, sizeof(double));
    double* sub = (double*)calloc((size_t)n, sizeof(double));

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst_tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        printf("%s: framebuffer incomplete 0x%x%s\n", tag, st, dst_rgba ? "" : " (R8 target rejected)");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        free(t);
        free(sub);
        return NULL;
    }

    glViewport(0, 0, g_w, g_h);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);

    glUseProgram(prog);
    glUniform2f(glGetUniformLocation(prog, "uSize"), (float)g_w, (float)g_h);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glUniform1i(glGetUniformLocation(prog, "uSrc"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, bayer_tex);
    glUniform1i(glGetUniformLocation(prog, "uBayer"), 1);

    // full-screen quad
    static const GLfloat verts[] = {
        -1.f, -1.f, 0.f, 0.f,
         1.f, -1.f, 1.f, 0.f,
        -1.f,  1.f, 0.f, 1.f,
         1.f,  1.f, 1.f, 1.f,
    };
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void*)(2 * sizeof(GLfloat)));

    for (int i = 0; i < g_warmup + n; i++) {
        double t0 = now_ms();
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glFlush();
        EGLSyncKHR sync = p_eglCreateSyncKHR(g_dpy, EGL_SYNC_FENCE_KHR, NULL);
        double t1 = now_ms();
        if (sync != EGL_NO_SYNC_KHR) {
            p_eglClientWaitSyncKHR(g_dpy, sync, 0, 1000000000ull /* 1 s */);
            p_eglDestroySyncKHR(g_dpy, sync);
        } else {
            glFinish();
        }
        double t2 = now_ms();
        if (i >= g_warmup) {
            sub[i - g_warmup] = t1 - t0;
            t[i - g_warmup] = t2 - t0;
        }
    }
    free(sub);  // submit time is inside `t`; keep the header simple

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteBuffers(1, &vbo);
    glDeleteFramebuffers(1, &fbo);

    report(tag, t, n, (double)g_w * g_h * 4 / 1e6);
    return t;
}

// compare GPU output against the CPU reference (`bpp` 1 for R8, 4 for RGBA)
static void verify(AHardwareBuffer* dst, const uint8_t* ref, const char* tag, int bpp) {
    if (!dst || !ref) return;
    void* p = NULL;
    if (AHardwareBuffer_lock(dst, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL, &p) != 0) {
        printf("%s: verify lock failed\n", tag);
        return;
    }
    AHardwareBuffer_Desc d;
    AHardwareBuffer_describe(dst, &d);
    int bad = 0, maxdiff = 0;
    for (uint32_t y = 0; y < d.height; y++) {
        const uint8_t* row = (const uint8_t*)p + (size_t)y * d.stride * bpp;
        const uint8_t* rr = ref + (size_t)y * g_w;
        for (uint32_t x = 0; x < d.width; x++) {
            int diff = (int)row[x * bpp] - (int)rr[x];
            if (diff < 0) diff = -diff;
            if (diff) { bad++; if (diff > maxdiff) maxdiff = diff; }
        }
    }
    printf("%s: %d/%d pixels differ from the CPU converter (max %d)\n", tag, bad, g_w * g_h, maxdiff);
    int32_t fence = -1;
    AHardwareBuffer_unlock(dst, &fence);
}

// ---------------------------------------------------------------------------
// cross-process lock: the case the composer is in
//
// The composer receives the client target's handle from SurfaceFlinger, so it
// did not allocate it. gralloc4 requires a foreign handle to be *imported* in
// the receiving process before lock(); AOSP does exactly that in
// GraphicBuffer::unflatten() (importBuffer(), then lock the imported handle).
// Calling lock() on an un-imported handle faulted inside gralloc.rogue.so in the
// composer (SIGSEGV at 0x8, MapperImpl_v4_0::lock).
//
// Also measured here: gralloc.rogue's lock path flushes through the PowerVR
// services client (libsrv_um PVRSRVGetClientEventFilter / PVRSRVMapToDeviceAddress),
// so the consumer must have its GPU client up. That rules out fork() (a forked
// child inherits a broken PVR state and faults in eglInitialize) and is why this
// test runs two separate processes over a unix socket.
//
//   --xproc            consumer: spawns the producer, receives the handle, inits
//                      EGL, then times lock+full 4 MB read 5 times
//   --xproc-producer   producer: allocates a render-target-shaped RGBA buffer
//                      and sends its handle
//
// The consumer never allocated the buffer, so its numbers are what the shim's
// read of the real client target should cost.
extern char** environ;
static volatile uint32_t g_xsink;

#define XLOG(...) do { dprintf(2, __VA_ARGS__); } while (0)

static int sock_addr(struct sockaddr_un* a, const char* path) {
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    snprintf(a->sun_path, sizeof(a->sun_path), "%s", path);
    return (int)sizeof(*a);
}

static int xproc_producer(const char* path) {
    unlink(path);
    int ls = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (ls < 0) { XLOG("xproc producer: socket: %s\n", strerror(errno)); return 1; }
    struct sockaddr_un a;
    int alen = sock_addr(&a, path);
    if (bind(ls, (struct sockaddr*)&a, alen) != 0) {
        XLOG("xproc producer: bind %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (listen(ls, 1) != 0) { XLOG("xproc producer: listen: %s\n", strerror(errno)); return 1; }

    uint64_t usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                     AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                     AHARDWAREBUFFER_USAGE_CPU_READ_RARELY |
                     AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY;
    AHardwareBuffer* src = NULL;
    if (alloc_buf(&src, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, usage, "xproc src") != 0)
        return 1;
    fill_source(src);

    int c = accept(ls, NULL, NULL);
    if (c < 0) { XLOG("xproc producer: accept: %s\n", strerror(errno)); return 1; }
    int r = AHardwareBuffer_sendHandleToUnixSocket(src, c);
    XLOG("xproc producer: sent handle r=%d\n", r);
    close(c);
    close(ls);
    unlink(path);
    AHardwareBuffer_release(src);
    return r == 0 ? 0 : 1;
}

static int xproc_test(void) {
    const char* path = "/data/local/tmp/xproc.sock";
    unlink(path);

    char self[256];
    ssize_t sn = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (sn <= 0) snprintf(self, sizeof(self), "/data/local/tmp/gpu_gray_bench");
    else self[sn] = 0;

    char* argv[] = { self, (char*)"--xproc-producer", (char*)path, NULL };
    pid_t pid = -1;
    int pr = posix_spawn(&pid, self, NULL, NULL, argv, environ);
    if (pr != 0) { XLOG("xproc: spawn failed %d\n", pr); return 1; }

    int s = -1;
    struct sockaddr_un a;
    int alen = sock_addr(&a, path);
    for (int i = 0; i < 100 && s < 0; i++) {
        int t = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (t < 0) break;
        if (connect(t, (struct sockaddr*)&a, alen) == 0) s = t;
        else { close(t); usleep(50000); }
    }
    if (s < 0) { XLOG("xproc: connect %s failed: %s\n", path, strerror(errno)); return 1; }

    AHardwareBuffer* b = NULL;
    int r = AHardwareBuffer_recvHandleFromUnixSocket(s, &b);
    close(s);
    if (r != 0 || !b) { XLOG("xproc: recv failed %d\n", r); return 1; }
    AHardwareBuffer_Desc d;
    AHardwareBuffer_describe(b, &d);
    XLOG("xproc: received fmt=0x%x %ux%u stride=%u usage=0x%llx\n", d.format, d.width, d.height,
         d.stride, (unsigned long long)d.usage);

    if (egl_setup() != 0) { XLOG("xproc: no GPU client; lock would fault\n"); return 1; }

    for (int n = 0; n < 5; n++) {
        void* p = NULL;
        double t0 = now_ms();
        if (AHardwareBuffer_lock(b, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL, &p) != 0) {
            XLOG("xproc: lock failed\n");
            return 1;
        }
        double t1 = now_ms();
        const uint32_t* q = (const uint32_t*)p;
        size_t words = (size_t)d.stride * d.height;
        uint32_t acc = 0;
        for (size_t k = 0; k < words; k++) acc += q[k];
        g_xsink = acc;
        double t2 = now_ms();
        int32_t fence = -1;
        AHardwareBuffer_unlock(b, &fence);
        XLOG("xproc: lock %6.2f  read %6.2f  total %6.2f ms  (%5.0f MB/s)\n", t1 - t0, t2 - t1,
             t2 - t0, (double)d.stride * d.height * 4 / 1e6 / ((t2 - t0) / 1000.0));
    }
    AHardwareBuffer_release(b);
    int st = 0;
    waitpid(pid, &st, 0);
    return 0;
}

static void usage(const char* argv0) {
    printf("usage: %s [--w N] [--h N] [--iters N] [--warmup N] [--cpu-iters N]\n"
           "          [--src-cached] [--no-verify]\n", argv0);
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--w") && i + 1 < argc) g_w = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--h") && i + 1 < argc) g_h = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc) g_iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--warmup") && i + 1 < argc) g_warmup = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cpu-iters") && i + 1 < argc) g_cpu_iters = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--src-cached")) g_src_cached = 1;
        else if (!strcmp(argv[i], "--no-verify")) g_verify = 0;
        else if (!strcmp(argv[i], "--xproc")) return xproc_test();
        else if (!strcmp(argv[i], "--xproc-producer") && i + 1 < argc) return xproc_producer(argv[++i]);
        else { usage(argv[0]); return 2; }
    }
    if (g_iters < 1) g_iters = 1;
    if (g_cpu_iters < 1) g_cpu_iters = 1;

    printf("gpu-gray: %dx%d  %.2f MB RGBA -> %.2f MB gray8\n", g_w, g_h,
           (double)g_w * g_h * 4 / 1e6, (double)g_w * g_h / 1e6);

    uint64_t src_usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                         AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |    // it is a render target in real life
                         (g_src_cached ? (AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
                                          AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN)
                                       : (AHARDWAREBUFFER_USAGE_CPU_READ_RARELY |
                                          AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY));
    uint64_t dst_usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                         AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                         AHARDWAREBUFFER_USAGE_CPU_READ_RARELY;

    AHardwareBuffer* src = NULL;
    AHardwareBuffer* dst8 = NULL;
    AHardwareBuffer* dst32 = NULL;
    if (alloc_buf(&src, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, src_usage, "src RGBA") != 0) return 1;
    // R8 is not universally allocatable; try the usages that matter in order.
    static const uint64_t kR8Usage[] = {
        AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY,
        AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
            AHARDWAREBUFFER_USAGE_CPU_READ_RARELY,
        AHARDWAREBUFFER_USAGE_CPU_READ_RARELY,
    };
    for (size_t i = 0; i < sizeof(kR8Usage) / sizeof(kR8Usage[0]) && !dst8; i++) {
        if (alloc_buf(&dst8, AHARDWAREBUFFER_FORMAT_R8_UNORM, kR8Usage[i], "dst R8") != 0) dst8 = NULL;
    }
    alloc_buf(&dst32, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, dst_usage, "dst RGBA");
    fill_source(src);

    // reference + CPU baselines
    uint8_t* ref = (uint8_t*)malloc((size_t)g_w * g_h);
    bench_cpu("[1] cpu  gray -> malloc", src, ref, g_w, NULL);
    if (dst8) {
        AHardwareBuffer_Desc d;
        AHardwareBuffer_describe(dst8, &d);
        void* dp = NULL;
        if (AHardwareBuffer_lock(dst8, AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY, -1, NULL, &dp) == 0) {
            bench_cpu("[2] cpu  gray -> gralloc R8", src, (uint8_t*)dp, (int)d.stride, NULL);
            int32_t fence = -1;
            AHardwareBuffer_unlock(dst8, &fence);
        }
    } else {
        printf("%-28s skipped: this allocator has no R8\n", "[2] cpu  gray -> gralloc R8");
    }
    uint8_t* big = (uint8_t*)malloc((size_t)g_w * g_h * 4);
    if (big) { bench_copy(src, big); free(big); }

    if (egl_setup() != 0) { free(ref); return 1; }
    GLuint prog = make_program();
    if (!prog) { free(ref); return 1; }
    GLuint bayer = make_bayer_texture();

    EGLImageKHR src_img = make_image(src);
    if (src_img == EGL_NO_IMAGE_KHR) { free(ref); return 1; }
    GLuint src_tex = image_texture(src_img);

    if (dst8) {
        EGLImageKHR img = make_image(dst8);
        if (img != EGL_NO_IMAGE_KHR) {
            GLuint tex = image_texture(img);
            double* t = bench_gpu("[4] gpu  gray -> gralloc R8", prog, src_tex, bayer, tex, 0);
            if (t && g_verify) {
                glFinish();
                verify(dst8, ref, "    verify R8", 1);
            }
            free(t);
        }
    }
    if (dst32) {
        EGLImageKHR img = make_image(dst32);
        if (img != EGL_NO_IMAGE_KHR) {
            GLuint tex = image_texture(img);
            double* t = bench_gpu("[5] gpu  gray -> gralloc RGBA", prog, src_tex, bayer, tex, 1);
            if (t && g_verify) {
                glFinish();
                verify(dst32, ref, "    verify RGBA", 4);
            }
            free(t);
        }
    }

    printf("\nHow to read this: [1] is the cost the shim pays today for the RGBA read\n"
           "(the vendor's staging copy, 31-80 ms in FINDING-fast-commit.md). If [4] is a\n"
           "few ms and the verify line is 0, a GPU pass inside the composer replaces that\n"
           "copy with the same maths, and the 4 MB read stops being the ceiling.\n");
    free(ref);
    return 0;
}
