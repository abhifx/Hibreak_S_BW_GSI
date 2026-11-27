// gray_helper -- standalone cross-process RGBA8888 -> packed gray8 converter.
//
// The helper owns a four-entry RGBA8888 AHardwareBuffer backing pool. A client
// sends a fixed GrayRequest packet, then an AHardwareBuffer handle. The helper
// initializes EGL (PowerVR), converts with GLES2 or a NEON fallback, writes
// exactly width*height packed gray bytes at offset 0, and sends the RGBA backing
// handle back. It never loads or modifies the HWC shim or SurfaceFlinger.

#define LOG_TAG "hwcgray"

#include <android/hardware_buffer.h>
#include <android/log.h>
#include <arm_neon.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

namespace {

constexpr const char* kDefaultSocket = "/data/local/tmp/hwcgray.sock";
constexpr uint32_t kPoolSize = 4;
constexpr int kSocketTimeoutSeconds = 5;
constexpr useconds_t kConnectRetryMicros = 50000;

// Android/AArch64 is little-endian. These integer values spell HWG1 and HWE1
// when written as bytes. All protocol fields are native little-endian.
constexpr uint32_t kRequestMagic = 0x31475748u;  // 'H' 'W' 'G' '1'
constexpr uint32_t kErrorMagic = 0x31455748u;    // 'H' 'W' 'E' '1'
constexpr uint32_t kRawTargetMagic = 0x314f5748u;  // 'H' 'W' 'O' '1'
constexpr uint32_t kGbMagic = 0x47423031u;         // 'G' 'B' '0' '1'
constexpr uint32_t kProtocolVersion = 2;

#pragma pack(push, 1)
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

struct GrayError {
    uint32_t magic;
    uint32_t version;
    int32_t status;
    int32_t native_error;
    uint32_t reserved[2];
};
#pragma pack(pop)

static_assert(sizeof(GrayRequest) == 40, "GrayRequest wire size changed");
static_assert(sizeof(GrayError) == 24, "GrayError wire size changed");

enum GrayStatus : int32_t {
    STATUS_OK = 0,
    STATUS_PROTOCOL = 1,
    STATUS_IO = 2,
    STATUS_EGL = 3,
    STATUS_ALLOCATE = 4,
    STATUS_IMPORT = 5,
    STATUS_INVALID_INPUT = 6,
    STATUS_GEOMETRY_MISMATCH = 7,
    STATUS_LOCK_INPUT = 8,
    STATUS_LOCK_OUTPUT = 9,
    STATUS_UNLOCK_INPUT = 10,
    STATUS_UNLOCK_OUTPUT = 11,
    STATUS_SEND_OUTPUT = 12,
    STATUS_RAW_TARGET = 13,
    STATUS_RAW_MMAP = 14,
    STATUS_RAW_COPY = 15,
    STATUS_RAW_MSYNC = 16,
    STATUS_RAW_MUNMAP = 17,
};

constexpr uint64_t kCpuRareUsage = AHARDWAREBUFFER_USAGE_CPU_READ_RARELY |
                                   AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY;

enum class ConverterMode { AUTO, CPU };
ConverterMode g_converter_mode = ConverterMode::AUTO;

// 4x4 Bayer thresholds, repeated across each 16-pixel NEON block. This is the
// same 0..15 ordered matrix and quantisation used by hwcshim_mt6765.cpp.
alignas(16) static const uint8_t kBayer4[4][16] = {
    { 0, 8, 2, 10, 0, 8, 2, 10, 0, 8, 2, 10, 0, 8, 2, 10 },
    { 12, 4, 14, 6, 12, 4, 14, 6, 12, 4, 14, 6, 12, 4, 14, 6 },
    { 3, 11, 1, 9, 3, 11, 1, 9, 3, 11, 1, 9, 3, 11, 1, 9 },
    { 15, 7, 13, 5, 15, 7, 13, 5, 15, 7, 13, 5, 15, 7, 13, 5 },
};

// Recursive 8x8 Bayer matrix, values 0..63. onebit mode thresholds at 128.
alignas(16) static const uint8_t kBayer8[8][8] = {
    { 0, 32, 8, 40, 2, 34, 10, 42 },
    { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44, 4, 36, 14, 46, 6, 38 },
    { 60, 28, 52, 20, 62, 30, 54, 22 },
    { 3, 35, 11, 43, 1, 33, 9, 41 },
    { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47, 7, 39, 13, 45, 5, 37 },
    { 63, 31, 55, 23, 61, 29, 53, 21 },
};

bool valid_geometry(uint32_t width, uint32_t height) {
    // Keep loop indices and row arithmetic comfortably representable.
    return width != 0 && height != 0 && width <= INT32_MAX && height <= INT32_MAX;
}

void gray16_neon(uint8_t* dst, const uint8_t* src, uint32_t width, uint32_t height,
                 size_t src_pitch, size_t dst_pitch) {
    const uint16x8_t fifteen = vdupq_n_u16(15);
    const uint16x8_t seventeen = vdupq_n_u16(17);

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* s = src + static_cast<size_t>(y) * src_pitch;
        uint8_t* d = dst + static_cast<size_t>(y) * dst_pitch;
        const uint8x16_t bayer = vld1q_u8(kBayer4[y & 3]);
        const uint16x8_t bayer_lo = vmovl_u8(vget_low_u8(bayer));
        const uint16x8_t bayer_hi = vmovl_u8(vget_high_u8(bayer));

        uint32_t x = 0;
        for (; x + 16 <= width; x += 16) {
            const uint8x16x4_t px = vld4q_u8(s + static_cast<size_t>(x) * 4);

            uint16x8_t lo = vmulq_n_u16(vmovl_u8(vget_low_u8(px.val[0])), 77);
            lo = vmlaq_n_u16(lo, vmovl_u8(vget_low_u8(px.val[1])), 150);
            lo = vmlaq_n_u16(lo, vmovl_u8(vget_low_u8(px.val[2])), 29);

            uint16x8_t hi = vmulq_n_u16(vmovl_u8(vget_high_u8(px.val[0])), 77);
            hi = vmlaq_n_u16(hi, vmovl_u8(vget_high_u8(px.val[1])), 150);
            hi = vmlaq_n_u16(hi, vmovl_u8(vget_high_u8(px.val[2])), 29);

            lo = vshrq_n_u16(vaddq_u16(vshrq_n_u16(lo, 8), bayer_lo), 4);
            hi = vshrq_n_u16(vaddq_u16(vshrq_n_u16(hi, 8), bayer_hi), 4);
            lo = vminq_u16(lo, fifteen);
            hi = vminq_u16(hi, fifteen);

            const uint8x16_t out = vcombine_u8(vmovn_u16(vmulq_u16(lo, seventeen)),
                                               vmovn_u16(vmulq_u16(hi, seventeen)));
            vst1q_u8(d + x, out);
        }

        for (; x < width; ++x) {
            const uint8_t* px = s + static_cast<size_t>(x) * 4;
            const uint32_t luma =
                (static_cast<uint32_t>(px[0]) * 77u + static_cast<uint32_t>(px[1]) * 150u +
                 static_cast<uint32_t>(px[2]) * 29u) >> 8;
            uint32_t q = (luma + kBayer4[y & 3][x & 15]) >> 4;
            if (q > 15) q = 15;
            d[x] = static_cast<uint8_t>(q * 17u);
        }
    }
}

void gray1_neon(uint8_t* dst, const uint8_t* src, uint32_t width, uint32_t height,
                size_t src_pitch, size_t dst_pitch) {
    const uint16x8_t one = vdupq_n_u16(1);
    const uint16x8_t full = vdupq_n_u16(255);

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* s = src + static_cast<size_t>(y) * src_pitch;
        uint8_t* d = dst + static_cast<size_t>(y) * dst_pitch;

        uint8_t thresholds[16];
        memcpy(thresholds, kBayer8[y & 7], 8);
        memcpy(thresholds + 8, kBayer8[y & 7], 8);
        const uint8x16_t bayer = vld1q_u8(thresholds);
        const uint16x8_t bayer_lo = vmovl_u8(vget_low_u8(bayer));
        const uint16x8_t bayer_hi = vmovl_u8(vget_high_u8(bayer));

        uint32_t x = 0;
        for (; x + 16 <= width; x += 16) {
            const uint8x16x4_t px = vld4q_u8(s + static_cast<size_t>(x) * 4);

            uint16x8_t lo = vmulq_n_u16(vmovl_u8(vget_low_u8(px.val[0])), 77);
            lo = vmlaq_n_u16(lo, vmovl_u8(vget_low_u8(px.val[1])), 150);
            lo = vmlaq_n_u16(lo, vmovl_u8(vget_low_u8(px.val[2])), 29);
            lo = vshrq_n_u16(lo, 8);

            uint16x8_t hi = vmulq_n_u16(vmovl_u8(vget_high_u8(px.val[0])), 77);
            hi = vmlaq_n_u16(hi, vmovl_u8(vget_high_u8(px.val[1])), 150);
            hi = vmlaq_n_u16(hi, vmovl_u8(vget_high_u8(px.val[2])), 29);
            hi = vshrq_n_u16(hi, 8);

            lo = vshrq_n_u16(vaddq_u16(lo, bayer_lo), 7);
            hi = vshrq_n_u16(vaddq_u16(hi, bayer_hi), 7);
            lo = vminq_u16(lo, one);
            hi = vminq_u16(hi, one);
            const uint8x16_t out = vcombine_u8(vmovn_u16(vmulq_u16(lo, full)),
                                               vmovn_u16(vmulq_u16(hi, full)));
            vst1q_u8(d + x, out);
        }

        for (; x < width; ++x) {
            const uint8_t* px = s + static_cast<size_t>(x) * 4;
            const uint32_t luma =
                (static_cast<uint32_t>(px[0]) * 77u + static_cast<uint32_t>(px[1]) * 150u +
                 static_cast<uint32_t>(px[2]) * 29u) >> 8;
            d[x] = (luma + kBayer8[y & 7][x & 7] >= 128) ? 255 : 0;
        }
    }
}

void gray_convert(uint8_t* dst, const uint8_t* src, uint32_t width, uint32_t height,
                  size_t src_pitch, size_t dst_pitch, uint32_t onebit) {
    if (onebit) {
        gray1_neon(dst, src, width, height, src_pitch, dst_pitch);
    } else {
        gray16_neon(dst, src, width, height, src_pitch, dst_pitch);
    }
}

struct EglState {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    bool ready = false;
};

EglState g_egl;

typedef EGLClientBuffer (*PFNGetNativeClientBuffer)(const void*);

struct GpuState {
    PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_texture = nullptr;
    PFNGetNativeClientBuffer get_native_client_buffer = nullptr;

    GLuint program = 0;
    GLuint bayer16 = 0;
    GLuint bayer64 = 0;
    GLuint vertex_buffer = 0;
    GLint uniform_source = -1;
    GLint uniform_bayer16 = -1;
    GLint uniform_bayer64 = -1;
    GLint uniform_size = -1;
    GLint uniform_backing_stride = -1;
    GLint uniform_onebit = -1;
    bool extensions_ready = false;
    bool resources_ready = false;
    bool resources_failed = false;
};

GpuState g_gpu;

void egl_cleanup() {
    if (g_egl.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_egl.context != EGL_NO_CONTEXT) {
            eglDestroyContext(g_egl.display, g_egl.context);
        }
        if (g_egl.surface != EGL_NO_SURFACE) {
            eglDestroySurface(g_egl.display, g_egl.surface);
        }
        eglTerminate(g_egl.display);
    }
    g_egl = EglState{};
}

int egl_failure(const char* stage) {
    EGLint error = eglGetError();
    if (error == EGL_SUCCESS) error = EIO;
    LOGE("EGL %s failed: 0x%x", stage, error);
    egl_cleanup();
    return static_cast<int>(error);
}

int ensure_egl() {
    if (g_egl.ready) return 0;

    g_egl.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_egl.display == EGL_NO_DISPLAY) return egl_failure("eglGetDisplay");

    EGLint major = 0;
    EGLint minor = 0;
    if (!eglInitialize(g_egl.display, &major, &minor)) return egl_failure("eglInitialize");

    const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(g_egl.display, config_attrs, &config, 1, &count) || count < 1) {
        return egl_failure("eglChooseConfig");
    }

    const EGLint pbuffer_attrs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    g_egl.surface = eglCreatePbufferSurface(g_egl.display, config, pbuffer_attrs);
    if (g_egl.surface == EGL_NO_SURFACE) return egl_failure("eglCreatePbufferSurface");

    if (!eglBindAPI(EGL_OPENGL_ES_API)) return egl_failure("eglBindAPI");
    const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    g_egl.context = eglCreateContext(g_egl.display, config, EGL_NO_CONTEXT, context_attrs);
    if (g_egl.context == EGL_NO_CONTEXT) return egl_failure("eglCreateContext");

    if (!eglMakeCurrent(g_egl.display, g_egl.surface, g_egl.surface, g_egl.context)) {
        return egl_failure("eglMakeCurrent");
    }

    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    g_gpu.create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    g_gpu.destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    g_gpu.image_target_texture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    g_gpu.get_native_client_buffer = reinterpret_cast<PFNGetNativeClientBuffer>(
        eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    g_gpu.extensions_ready = g_gpu.create_image && g_gpu.destroy_image &&
                             g_gpu.image_target_texture &&
                             g_gpu.get_native_client_buffer;
    if (!g_gpu.extensions_ready) {
        LOGE("EGL image extensions unavailable; using CPU NEON fallback");
    }

    LOGI("EGL %d.%d ready; GL %s; renderer %s", major, minor,
         version ? version : "unknown", renderer ? renderer : "unknown");
    g_egl.ready = true;
    return 0;
}

static const char kGpuVertex[] =
    "attribute vec2 aPos;\n"
    "void main() { gl_Position = vec4(aPos, 0.0, 1.0); }\n";

static const char kGpuFragment[] =
    "precision highp float;\n"
    "uniform sampler2D uSrc;\n"
    "uniform sampler2D uBayer16;\n"
    "uniform sampler2D uBayer64;\n"
    "uniform vec2 uSize;\n"
    "uniform float uBackingStride;\n"
    "uniform float uOnebit;\n"
    "float luma(vec4 c) {\n"
    "    float r = floor(c.r + 0.5);\n"
    "    float g = floor(c.g + 0.5);\n"
    "    float b = floor(c.b + 0.5);\n"
    "    return floor((r * 77.0 + g * 150.0 + b * 29.0) * (1.0 / 256.0));\n"
    "}\n"
    "float threshold16(vec2 p) {\n"
    "    vec2 q = mod(p, vec2(4.0, 4.0));\n"
    "    return floor(texture2D(uBayer16, (q + 0.5) * 0.25).r * 255.0 + 0.5);\n"
    "}\n"
    "float threshold64(vec2 p) {\n"
    "    vec2 q = mod(p, vec2(8.0, 8.0));\n"
    "    return floor(texture2D(uBayer64, (q + 0.5) * 0.125).r * 255.0 + 0.5);\n"
    "}\n"
    "float gray_value(vec2 p, vec4 c) {\n"
    "    float l = luma(c);\n"
    "    if (uOnebit > 0.5) {\n"
    "        return (l + threshold64(p) >= 128.0) ? 1.0 : 0.0;\n"
    "    }\n"
    "    float q = min(floor((l + threshold16(p)) * (1.0 / 16.0)), 15.0);\n"
    "    return q * 17.0 * (1.0 / 255.0);\n"
    "}\n"
    "void main() {\n"
    "    vec2 physical_px = floor(gl_FragCoord.xy);\n"
    "    float linear_px = physical_px.y * uBackingStride + physical_px.x;\n"
    "    float packed_columns = uSize.x * 0.25;\n"
    "    float packed_row = floor(linear_px / packed_columns);\n"
    "    if (packed_row >= uSize.y) discard;\n"
    "    float packed_column = linear_px - packed_row * packed_columns;\n"
    "    vec2 base = vec2(packed_column * 4.0, packed_row);\n"
    "    vec2 inv_size = 1.0 / uSize;\n"
    "    vec4 c0 = texture2D(uSrc, (base + vec2(0.5, 0.5)) * inv_size) * 255.0;\n"
    "    vec4 c1 = texture2D(uSrc, (base + vec2(1.5, 0.5)) * inv_size) * 255.0;\n"
    "    vec4 c2 = texture2D(uSrc, (base + vec2(2.5, 0.5)) * inv_size) * 255.0;\n"
    "    vec4 c3 = texture2D(uSrc, (base + vec2(3.5, 0.5)) * inv_size) * 255.0;\n"
    "    gl_FragColor = vec4(gray_value(base + vec2(0.0, 0.0), c0),\n"
    "                       gray_value(base + vec2(1.0, 0.0), c1),\n"
    "                       gray_value(base + vec2(2.0, 0.0), c2),\n"
    "                       gray_value(base + vec2(3.0, 0.0), c3));\n"
    "}\n";

GLuint gpu_compile_shader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    if (!shader) return 0;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024] = {};
        glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        LOGE("GPU shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint gpu_make_program() {
    const GLuint vertex = gpu_compile_shader(GL_VERTEX_SHADER, kGpuVertex);
    const GLuint fragment = gpu_compile_shader(GL_FRAGMENT_SHADER, kGpuFragment);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return 0;
    }

    const GLuint program = glCreateProgram();
    if (!program) {
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        return 0;
    }
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "aPos");
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[1024] = {};
        glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
        LOGE("GPU program link failed: %s", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

GLuint gpu_make_bayer_texture(const uint8_t* data, GLsizei size) {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (!texture) return 0;
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, size, size, 0, GL_LUMINANCE,
                 GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &texture);
        return 0;
    }
    return texture;
}

bool gpu_make_vertex_buffer() {
    static const GLfloat vertices[] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    glGenBuffers(1, &g_gpu.vertex_buffer);
    if (!g_gpu.vertex_buffer) return false;
    glBindBuffer(GL_ARRAY_BUFFER, g_gpu.vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    return glGetError() == GL_NO_ERROR;
}

void gpu_cleanup_resources() {
    if (g_gpu.vertex_buffer) glDeleteBuffers(1, &g_gpu.vertex_buffer);
    if (g_gpu.bayer64) glDeleteTextures(1, &g_gpu.bayer64);
    if (g_gpu.bayer16) glDeleteTextures(1, &g_gpu.bayer16);
    if (g_gpu.program) glDeleteProgram(g_gpu.program);
    g_gpu.program = 0;
    g_gpu.bayer16 = 0;
    g_gpu.bayer64 = 0;
    g_gpu.vertex_buffer = 0;
    g_gpu.uniform_source = -1;
    g_gpu.uniform_bayer16 = -1;
    g_gpu.uniform_bayer64 = -1;
    g_gpu.uniform_size = -1;
    g_gpu.uniform_backing_stride = -1;
    g_gpu.uniform_onebit = -1;
    g_gpu.resources_ready = false;
}

void gpu_cleanup() {
    gpu_cleanup_resources();
    g_gpu.resources_failed = false;
    g_gpu.extensions_ready = false;
}

bool ensure_gpu_resources() {
    if (g_gpu.resources_ready) return true;
    if (g_gpu.resources_failed) return false;

    uint8_t bayer16[16];
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 4; ++x) bayer16[y * 4 + x] = kBayer4[y][x];
    }

    g_gpu.program = gpu_make_program();
    g_gpu.bayer16 = gpu_make_bayer_texture(bayer16, 4);
    g_gpu.bayer64 = gpu_make_bayer_texture(&kBayer8[0][0], 8);
    const bool vertex_ok = gpu_make_vertex_buffer();
    if (!g_gpu.program || !g_gpu.bayer16 || !g_gpu.bayer64 || !vertex_ok) {
        LOGE("GPU resource setup failed; using CPU NEON fallback");
        gpu_cleanup_resources();
        g_gpu.resources_failed = true;
        return false;
    }

    g_gpu.uniform_source = glGetUniformLocation(g_gpu.program, "uSrc");
    g_gpu.uniform_bayer16 = glGetUniformLocation(g_gpu.program, "uBayer16");
    g_gpu.uniform_bayer64 = glGetUniformLocation(g_gpu.program, "uBayer64");
    g_gpu.uniform_size = glGetUniformLocation(g_gpu.program, "uSize");
    g_gpu.uniform_backing_stride = glGetUniformLocation(g_gpu.program, "uBackingStride");
    g_gpu.uniform_onebit = glGetUniformLocation(g_gpu.program, "uOnebit");
    if (g_gpu.uniform_source < 0 || g_gpu.uniform_bayer16 < 0 ||
        g_gpu.uniform_bayer64 < 0 || g_gpu.uniform_size < 0 ||
        g_gpu.uniform_backing_stride < 0 || g_gpu.uniform_onebit < 0) {
        LOGE("GPU uniform setup failed; using CPU NEON fallback");
        gpu_cleanup_resources();
        g_gpu.resources_failed = true;
        return false;
    }

    g_gpu.resources_ready = true;
    LOGI("GPU packed-gray converter ready");
    return true;
}

EGLImageKHR gpu_make_image(AHardwareBuffer* buffer) {
    if (!g_gpu.extensions_ready) return EGL_NO_IMAGE_KHR;
    EGLClientBuffer native = g_gpu.get_native_client_buffer(buffer);
    if (!native) return EGL_NO_IMAGE_KHR;
    const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    return g_gpu.create_image(g_egl.display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                              native, attrs);
}

GLuint gpu_image_texture(EGLImageKHR image) {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    if (!texture) return 0;
    glBindTexture(GL_TEXTURE_2D, texture);
    g_gpu.image_target_texture(GL_TEXTURE_2D, reinterpret_cast<GLeglImageOES>(image));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &texture);
        return 0;
    }
    return texture;
}

struct GpuRequestResources {
    EGLImageKHR source_image = EGL_NO_IMAGE_KHR;
    EGLImageKHR output_image = EGL_NO_IMAGE_KHR;
    GLuint source_texture = 0;
    GLuint output_texture = 0;
    GLuint framebuffer = 0;

    ~GpuRequestResources() {
        if (framebuffer) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &framebuffer);
        }
        if (output_texture) glDeleteTextures(1, &output_texture);
        if (source_texture) glDeleteTextures(1, &source_texture);
        if (output_image != EGL_NO_IMAGE_KHR) g_gpu.destroy_image(g_egl.display, output_image);
        if (source_image != EGL_NO_IMAGE_KHR) g_gpu.destroy_image(g_egl.display, source_image);
    }
};

struct BufferPool {
    AHardwareBuffer* buffers[kPoolSize]{};
    EGLImageKHR output_images[kPoolSize]{};
    GLuint output_textures[kPoolSize]{};
    GLuint framebuffers[kPoolSize]{};
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t next = 0;
    uint64_t usage = 0;
    bool initialized = false;
};

BufferPool g_pool;
bool g_accum_valid = false;
uint32_t g_accum_width = 0;
uint32_t g_accum_height = 0;
uint8_t g_accum_onebit = 0;

bool request_damage(const GrayRequest& request, uint32_t* left, uint32_t* top,
                    uint32_t* right, uint32_t* bottom) {
    if (request.left == UINT32_MAX && request.top == UINT32_MAX &&
        request.right == UINT32_MAX && request.bottom == UINT32_MAX) return false;
    if (request.left >= request.width || request.top >= request.height ||
        request.right >= request.width || request.bottom >= request.height ||
        request.right < request.left || request.bottom < request.top ||
        (request.width % 4) != 0) return false;
    *left = request.left;
    *top = request.top;
    *right = request.right;
    *bottom = request.bottom;
    return true;
}

bool useful_partial_damage(const GrayRequest& request) {
    uint32_t left, top, right, bottom;
    if (request.reserved != 1 || !request_damage(request, &left, &top, &right, &bottom)) return false;
    const uint64_t changed = (uint64_t)(right - left + 1) * (bottom - top + 1);
    const uint64_t total = (uint64_t)request.width * request.height;
    return changed > 0 && changed * 4 < total * 3;
}

void release_pool_gpu() {
    if (!g_egl.ready) return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (uint32_t i = 0; i < kPoolSize; ++i) {
        if (g_pool.framebuffers[i]) {
            glDeleteFramebuffers(1, &g_pool.framebuffers[i]);
            g_pool.framebuffers[i] = 0;
        }
        if (g_pool.output_textures[i]) {
            glDeleteTextures(1, &g_pool.output_textures[i]);
            g_pool.output_textures[i] = 0;
        }
        if (g_pool.output_images[i] != EGL_NO_IMAGE_KHR && g_gpu.destroy_image) {
            g_gpu.destroy_image(g_egl.display, g_pool.output_images[i]);
            g_pool.output_images[i] = EGL_NO_IMAGE_KHR;
        }
    }
    glFinish();
}

void release_pool() {
    release_pool_gpu();
    for (uint32_t i = 0; i < kPoolSize; ++i) {
        if (g_pool.buffers[i]) {
            AHardwareBuffer_release(g_pool.buffers[i]);
            g_pool.buffers[i] = nullptr;
        }
    }
    g_pool = BufferPool{};
    g_accum_valid = false;
    g_accum_width = 0;
    g_accum_height = 0;
    g_accum_onebit = 0;
    for (uint32_t i = 0; i < kPoolSize; ++i) {
        g_pool.output_images[i] = EGL_NO_IMAGE_KHR;
    }
}

bool pool_complete() {
    for (uint32_t i = 0; i < kPoolSize; ++i) {
        if (!g_pool.buffers[i]) return false;
    }
    return true;
}

bool valid_rgba_backing(const AHardwareBuffer_Desc& desc, uint32_t width, uint32_t height) {
    if (desc.format != AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM || desc.layers != 1 ||
        desc.width != width || desc.height != height || desc.stride < width ||
        width == 0 || height == 0 || width > SIZE_MAX / height) {
        return false;
    }
    // This helper targets arm64, where a uint32_t pixel stride times four fits
    // in size_t. The row-count check below still rejects size_t overflow.
    const size_t backing_pitch = static_cast<size_t>(desc.stride) * 4;
    if (desc.height > SIZE_MAX / backing_pitch) return false;
    const size_t backing_bytes = backing_pitch * desc.height;
    const size_t gray_bytes = static_cast<size_t>(width) * height;
    return backing_bytes >= gray_bytes;
}

bool ensure_pool_slot_gpu(uint32_t slot) {
    if (slot >= kPoolSize || !g_pool.buffers[slot]) return false;
    if (g_pool.framebuffers[slot] && g_pool.output_textures[slot] &&
        g_pool.output_images[slot] != EGL_NO_IMAGE_KHR) {
        return true;
    }
    if (g_pool.framebuffers[slot]) {
        glDeleteFramebuffers(1, &g_pool.framebuffers[slot]);
        g_pool.framebuffers[slot] = 0;
    }
    if (g_pool.output_textures[slot]) {
        glDeleteTextures(1, &g_pool.output_textures[slot]);
        g_pool.output_textures[slot] = 0;
    }
    if (g_pool.output_images[slot] != EGL_NO_IMAGE_KHR && g_gpu.destroy_image) {
        g_gpu.destroy_image(g_egl.display, g_pool.output_images[slot]);
        g_pool.output_images[slot] = EGL_NO_IMAGE_KHR;
    }

    EGLImageKHR image = gpu_make_image(g_pool.buffers[slot]);
    if (image == EGL_NO_IMAGE_KHR) return false;
    GLuint texture = gpu_image_texture(image);
    if (!texture) {
        g_gpu.destroy_image(g_egl.display, image);
        return false;
    }
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    if (!fbo) {
        glDeleteTextures(1, &texture);
        g_gpu.destroy_image(g_egl.display, image);
        return false;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &texture);
        g_gpu.destroy_image(g_egl.display, image);
        return false;
    }
    g_pool.output_images[slot] = image;
    g_pool.output_textures[slot] = texture;
    g_pool.framebuffers[slot] = fbo;
    return true;
}

bool try_gpu_convert(AHardwareBuffer* input, uint32_t output_index,
                     const GrayRequest& request) {
    if (g_converter_mode == ConverterMode::CPU || request.width % 4 != 0 ||
        output_index >= kPoolSize) return false;

    AHardwareBuffer* output = g_pool.buffers[output_index];
    AHardwareBuffer_Desc output_desc{};
    AHardwareBuffer_describe(output, &output_desc);
    if (!valid_rgba_backing(output_desc, request.width, request.height) ||
        output_desc.stride != request.width) {
        LOGD("GPU packed layout unavailable (stride=%u width=%u); using CPU",
             output_desc.stride, request.width);
        return false;
    }
    const size_t packed_pixels = static_cast<size_t>(request.width / 4) * request.height;
    const uint32_t physical_rows = static_cast<uint32_t>(
        (packed_pixels + output_desc.stride - 1) / output_desc.stride);
    uint32_t damage_left = 0, damage_top = 0, damage_right = 0, damage_bottom = 0;
    const bool partial = useful_partial_damage(request) &&
                         request_damage(request, &damage_left, &damage_top,
                                        &damage_right, &damage_bottom);
    if (!g_gpu.extensions_ready || !ensure_gpu_resources()) return false;

    (void)glGetError();
    if (!ensure_pool_slot_gpu(output_index)) {
        LOGD("output pool GPU setup failed; using CPU");
        return false;
    }

    GpuRequestResources resources;
    resources.source_image = gpu_make_image(input);
    if (resources.source_image == EGL_NO_IMAGE_KHR) {
        LOGD("input EGLImage creation failed; using CPU");
        return false;
    }
    resources.source_texture = gpu_image_texture(resources.source_image);
    if (!resources.source_texture) {
        LOGD("input EGLImage texture creation failed; using CPU");
        return false;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, g_pool.framebuffers[output_index]);

    // The logical target is width/4 x height RGBA pixels. The packed payload is
    // a dense prefix of the RGBA mapping, so render that physical prefix and
    // let the fragment shader map each four-byte fragment to its packed row.
    glViewport(0, 0, static_cast<GLint>(output_desc.stride),
               static_cast<GLint>(physical_rows));
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glUseProgram(g_gpu.program);
    glUniform2f(g_gpu.uniform_size, static_cast<float>(request.width),
                static_cast<float>(request.height));
    glUniform1f(g_gpu.uniform_backing_stride, static_cast<float>(output_desc.stride));
    glUniform1f(g_gpu.uniform_onebit, request.onebit ? 1.0f : 0.0f);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, resources.source_texture);
    glUniform1i(g_gpu.uniform_source, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g_gpu.bayer16);
    glUniform1i(g_gpu.uniform_bayer16, 1);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, g_gpu.bayer64);
    glUniform1i(g_gpu.uniform_bayer64, 2);

    glBindBuffer(GL_ARRAY_BUFFER, g_gpu.vertex_buffer);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    if (partial) {
        // A physical RGBA row contains four logical gray rows in four horizontal
        // bands. Update each intersecting band with its own scissor rectangle.
        const uint32_t packed_columns = request.width / 4;
        const uint32_t fragment_x0 = damage_left / 4;
        const uint32_t fragment_x1 = (damage_right + 1 + 3) / 4;
        glEnable(GL_SCISSOR_TEST);
        for (uint32_t band = 0; band < 4; ++band) {
            uint32_t first = damage_top + ((band + 4 - (damage_top & 3u)) & 3u);
            if (first > damage_bottom) continue;
            uint32_t last = first;
            while (last + 4 <= damage_bottom) last += 4;
            const GLint y0 = (GLint)(first / 4);
            const GLint rows = (GLint)((last - first) / 4 + 1);
            const GLint x0 = (GLint)(band * packed_columns + fragment_x0);
            const GLint x1 = (GLint)(band * packed_columns + fragment_x1);
            glScissor(x0, y0, x1 - x0, rows);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        glDisable(GL_SCISSOR_TEST);
    } else {
        glDisable(GL_SCISSOR_TEST);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glFinish();
    if (glGetError() != GL_NO_ERROR) {
        LOGD("GPU draw failed; using CPU");
        return false;
    }

    static uint64_t logged = 0;
    ++logged;
    if (logged <= 4 || (logged % 60) == 0)
        LOGD("GPU converted %ux%u onebit=%u logical=%ux%u physical_viewport=%ux%u",
             request.width, request.height, request.onebit, request.width / 4,
             request.height, output_desc.stride, physical_rows);
    return true;
}

int ensure_pool(uint32_t width, uint32_t height) {
    if (!g_pool.initialized) {
        g_pool.width = width;
        g_pool.height = height;
        g_pool.initialized = true;
    } else if (g_pool.width != width || g_pool.height != height) {
        // The helper is serial, so no request can be using the old pool here.
        // Rebuild instead of staying alive-but-broken for the rest of the boot.
        LOGI("geometry changed %ux%u -> %ux%u; rebuilding output pool",
             g_pool.width, g_pool.height, width, height);
        release_pool();
        g_pool.width = width;
        g_pool.height = height;
        g_pool.initialized = true;
    }

    if (pool_complete()) return 0;

    // Prefer an EGLImage-renderable RGBA backing while retaining cached CPU
    // access for NEON fallback. Add sampled-image usage only if needed, then
    // keep CPU-only allocation as the final compatibility fallback.
    static const uint64_t usages[] = {
        kCpuRareUsage | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER,
        kCpuRareUsage | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
            AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE,
        kCpuRareUsage,
    };

    int last_error = ENOMEM;
    for (uint64_t usage : usages) {
        AHardwareBuffer* allocated[kPoolSize]{};
        bool ok = true;
        for (uint32_t i = 0; i < kPoolSize; ++i) {
            AHardwareBuffer_Desc desc{};
            desc.width = width;
            desc.height = height;
            desc.layers = 1;
            desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
            desc.usage = usage;

            const int result = AHardwareBuffer_allocate(&desc, &allocated[i]);
            if (result != 0 || !allocated[i]) {
                last_error = result != 0 ? result : EINVAL;
                ok = false;
                break;
            }

            AHardwareBuffer_Desc got{};
            AHardwareBuffer_describe(allocated[i], &got);
            if (!valid_rgba_backing(got, width, height) ||
                (got.usage & kCpuRareUsage) != kCpuRareUsage) {
                last_error = EINVAL;
                ok = false;
                break;
            }
        }

        if (!ok) {
            for (AHardwareBuffer* buffer : allocated) {
                if (buffer) AHardwareBuffer_release(buffer);
            }
            continue;
        }

        for (uint32_t i = 0; i < kPoolSize; ++i) {
            g_pool.buffers[i] = allocated[i];
            g_pool.output_images[i] = EGL_NO_IMAGE_KHR;
            g_pool.output_textures[i] = 0;
            g_pool.framebuffers[i] = 0;
        }
        g_pool.usage = usage;
        AHardwareBuffer_Desc got{};
        AHardwareBuffer_describe(g_pool.buffers[0], &got);
        LOGI("RGBA gray-backing pool ready: 4 x %ux%u stride=%u gray_bytes=%zu "
             "backing_bytes=%zu usage=0x%llx", width, height, got.stride,
             static_cast<size_t>(width) * height,
             static_cast<size_t>(got.stride) * 4 * got.height,
             static_cast<unsigned long long>(usage));
        return 0;
    }

    LOGE("RGBA gray-backing pool allocation failed: %d", last_error);
    return last_error;
}

int send_error(int client, GrayStatus status, int32_t native_error) {
    GrayError error{};
    error.magic = kErrorMagic;
    error.version = kProtocolVersion;
    error.status = status;
    error.native_error = native_error;
    const ssize_t sent = send(client, &error, sizeof(error), MSG_NOSIGNAL);
    if (sent != static_cast<ssize_t>(sizeof(error))) {
        LOGE("could not send error packet: %s", strerror(errno));
        return -errno;
    }
    return 0;
}

int request_failure(int client, GrayStatus status, int32_t native_error, const char* what) {
    LOGE("%s: status=%d native=%d", what, static_cast<int>(status), native_error);
    send_error(client, status, native_error);
    return 1;
}

struct ScopedFd {
    int value = -1;
    ~ScopedFd() { reset(); }
    void reset() {
        if (value >= 0) close(value);
        value = -1;
    }
};

constexpr size_t kInputCacheSize = 4;
struct InputCacheEntry {
    bool valid = false;
    dev_t dev = 0;
    ino_t ino = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t format = 0;
    uint64_t usage = 0;
    AHardwareBuffer* buffer = nullptr;
};
InputCacheEntry g_input_cache[kInputCacheSize];
size_t g_input_cache_next = 0;

constexpr size_t kMaxGbWords = 13 + 128;
constexpr size_t kMaxGbFds = 128;
struct ReceivedGb01 {
    int32_t words[kMaxGbWords]{};
    int fds[kMaxGbFds]{};
    size_t word_count = 0;
    size_t fd_count = 0;
    ~ReceivedGb01() {
        for (size_t i = 0; i < fd_count; ++i) close(fds[i]);
    }
};

int receive_gb01(int client, ReceivedGb01* received) {
    alignas(cmsghdr) uint8_t control[CMSG_SPACE(kMaxGbFds * sizeof(int32_t))]{};
    iovec iov{received->words, sizeof(received->words)};
    msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);

    const ssize_t got = recvmsg(client, &message, 0);
    if (got < 0 || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) return -1;
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&message); cmsg; cmsg = CMSG_NXTHDR(&message, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len < CMSG_LEN(0)) continue;
        const size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
        if (bytes % sizeof(int32_t) != 0) return -1;
        const auto* fds = reinterpret_cast<const int32_t*>(CMSG_DATA(cmsg));
        const size_t count = bytes / sizeof(int32_t);
        if (count > kMaxGbFds - received->fd_count) return -1;
        for (size_t i = 0; i < count; ++i) received->fds[received->fd_count++] = fds[i];
    }
    if (got < (ssize_t)(13 * sizeof(int32_t)) || received->words[0] != (int32_t)kGbMagic ||
        received->words[10] <= 0 || received->words[10] > (int32_t)kMaxGbFds ||
        received->words[11] < 0 || received->words[11] > 128 ||
        received->fd_count != (size_t)received->words[10]) return -1;
    const size_t expected_words = 13 + (size_t)received->words[11];
    if ((size_t)got != expected_words * sizeof(int32_t)) return -1;
    received->word_count = expected_words;
    return 0;
}

AHardwareBuffer* acquire_input_buffer(int client) {
    ReceivedGb01 received;
    if (receive_gb01(client, &received) != 0) return nullptr;
    struct stat identity{};
    if (fstat(received.fds[0], &identity) != 0) return nullptr;
    const uint64_t usage = (uint32_t)received.words[6] |
                           ((uint64_t)(uint32_t)received.words[7] << 32);
    for (const auto& entry : g_input_cache) {
        if (entry.valid && entry.dev == identity.st_dev && entry.ino == identity.st_ino &&
            entry.width == (uint32_t)received.words[1] &&
            entry.height == (uint32_t)received.words[2] &&
            entry.stride == (uint32_t)received.words[3] &&
            entry.format == (uint32_t)received.words[4] && entry.usage == usage) {
            return entry.buffer;
        }
    }

    // Cache miss: forward the exact flattened handle through a private socket so
    // AHardwareBuffer performs its normal gralloc import, then retain it.
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) != 0) return nullptr;
    alignas(cmsghdr) uint8_t control[CMSG_SPACE(kMaxGbFds * sizeof(int32_t))]{};
    iovec iov{received.words, received.word_count * sizeof(int32_t)};
    msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    cmsghdr* cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(received.fd_count * sizeof(int32_t));
    memcpy(CMSG_DATA(cmsg), received.fds, received.fd_count * sizeof(int32_t));
    message.msg_controllen = cmsg->cmsg_len;
    const ssize_t sent = sendmsg(sockets[1], &message, MSG_NOSIGNAL);
    close(sockets[1]);
    AHardwareBuffer* buffer = nullptr;
    int result = sent == (ssize_t)(received.word_count * sizeof(int32_t))
                     ? AHardwareBuffer_recvHandleFromUnixSocket(sockets[0], &buffer)
                     : -1;
    close(sockets[0]);
    if (result != 0 || !buffer) {
        if (buffer) AHardwareBuffer_release(buffer);
        return nullptr;
    }

    InputCacheEntry& entry = g_input_cache[g_input_cache_next];
    g_input_cache_next = (g_input_cache_next + 1) % kInputCacheSize;
    if (entry.buffer) AHardwareBuffer_release(entry.buffer);
    entry = InputCacheEntry{};
    entry.valid = true;
    entry.dev = identity.st_dev;
    entry.ino = identity.st_ino;
    entry.width = (uint32_t)received.words[1];
    entry.height = (uint32_t)received.words[2];
    entry.stride = (uint32_t)received.words[3];
    entry.format = (uint32_t)received.words[4];
    entry.usage = usage;
    entry.buffer = buffer;
    return buffer;
}

void release_input_cache() {
    for (auto& entry : g_input_cache) {
        if (entry.buffer) AHardwareBuffer_release(entry.buffer);
        entry = InputCacheEntry{};
    }
}

int receive_raw_target_fd(int client) {
    uint32_t payload = 0;
    struct iovec iov {&payload, sizeof(payload)};
    alignas(struct cmsghdr) uint8_t control[CMSG_SPACE(sizeof(int32_t))]{};
    struct msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);

    const ssize_t received = recvmsg(client, &message, 0);
    const int receive_errno = errno;
    int raw_fd = -1;
    bool invalid = false;
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message); cmsg;
         cmsg = CMSG_NXTHDR(&message, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
        if (cmsg->cmsg_len < CMSG_LEN(0)) {
            invalid = true;
            continue;
        }
        const size_t fd_bytes = cmsg->cmsg_len - CMSG_LEN(0);
        if (fd_bytes % sizeof(int32_t) != 0 || fd_bytes != sizeof(int32_t)) invalid = true;
        const int32_t* fds = reinterpret_cast<const int32_t*>(CMSG_DATA(cmsg));
        const size_t fd_count = fd_bytes / sizeof(int32_t);
        for (size_t i = 0; i < fd_count; ++i) {
            if (raw_fd < 0) {
                raw_fd = fds[i];
            } else {
                close(fds[i]);
                invalid = true;
            }
        }
    }
    if ((message.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) != 0) invalid = true;

    if (received != static_cast<ssize_t>(sizeof(payload)) || payload != kRawTargetMagic ||
        raw_fd < 0 || invalid) {
        if (raw_fd >= 0) close(raw_fd);
        if (received < 0) {
            errno = receive_errno;
        } else {
            errno = invalid ? EINVAL : EMSGSIZE;
        }
        return -1;
    }

    const int flags = fcntl(raw_fd, F_GETFD);
    if (flags >= 0) fcntl(raw_fd, F_SETFD, flags | FD_CLOEXEC);
    return raw_fd;
}

int copy_packed_to_raw_fd(const uint8_t* source, int raw_fd, size_t bytes,
                          const GrayRequest& request, bool partial,
                          GrayStatus* failure_status) {
    if (!source || raw_fd < 0 || bytes == 0) {
        *failure_status = STATUS_RAW_COPY;
        return EINVAL;
    }

    struct stat info{};
    if (fstat(raw_fd, &info) != 0) {
        *failure_status = STATUS_RAW_TARGET;
        return errno;
    }
    if (info.st_size < 0 || static_cast<uint64_t>(info.st_size) < bytes) {
        *failure_status = STATUS_RAW_TARGET;
        return EINVAL;
    }

    void* target = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, raw_fd, 0);
    if (target == MAP_FAILED) {
        *failure_status = STATUS_RAW_MMAP;
        return errno;
    }

    // Vendor CommitBuffers rotate, so their contents outside the current damage
    // may be several frames old. GPU conversion may be partial, but the raw copy
    // must publish the complete accumulator to whichever exact fd HWC selected.
    (void)request;
    (void)partial;
    memcpy(target, source, bytes);
    int result = 0;
    if (msync(target, bytes, MS_SYNC) != 0) {
        // dma-buf mappings commonly reject msync with ENOTTY/EINVAL/ENOSYS even
        // though MAP_SHARED writes are coherent for the kernel/device. Tolerate
        // only those tested unsupported-operation errors; a real I/O failure
        // must not be acknowledged as a populated vendor target.
        const int saved = errno;
        if (saved == ENOTTY || saved == EINVAL || saved == ENOSYS) {
            static bool warned = false;
            if (!warned) {
                LOGD("raw target msync unsupported: %s", strerror(saved));
                warned = true;
            }
        } else {
            *failure_status = STATUS_RAW_MSYNC;
            result = saved;
        }
    }
    if (munmap(target, bytes) != 0 && result == 0) {
        *failure_status = STATUS_RAW_MUNMAP;
        result = errno;
    }
    return result;
}

int process_request(int client, const GrayRequest& request) {
    AHardwareBuffer* input = acquire_input_buffer(client);
    if (!input) {
        return request_failure(client, STATUS_IMPORT, EIO,
                               "input handle receive/import failed");
    }
    int result = 0;

    ScopedFd raw_target;
    if (request.reserved == 1) {
        raw_target.value = receive_raw_target_fd(client);
        if (raw_target.value < 0) {
            return request_failure(client, STATUS_RAW_TARGET, errno,
                                   "raw output-target packet invalid");
        }
    }

    AHardwareBuffer_Desc input_desc{};
    AHardwareBuffer_describe(input, &input_desc);
    if (input_desc.format != AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM ||
        input_desc.layers != 1 || input_desc.width != request.width ||
        input_desc.height != request.height || input_desc.stride < request.width ||
        input_desc.height > SIZE_MAX / (static_cast<size_t>(input_desc.stride) * 4)) {
        return request_failure(client, STATUS_INVALID_INPUT, EINVAL,
                               "input is not matching single-layer RGBA8888");
    }

    result = ensure_pool(request.width, request.height);
    if (result != 0) {
        const GrayStatus status = result == -EINVAL ? STATUS_GEOMETRY_MISMATCH
                                                    : STATUS_ALLOCATE;
        return request_failure(client, status, result, "output pool unavailable");
    }

    // gralloc.rogue's lock path uses the PowerVR services client. This must be
    // a real, current EGL context in this process before the first AHB lock.
    result = ensure_egl();
    if (result != 0) {
        return request_failure(client, STATUS_EGL, result, "PowerVR/EGL init failed");
    }

    const size_t gray_bytes = static_cast<size_t>(request.width) * request.height;
    GrayRequest effective = request;
    const bool raw = raw_target.value >= 0;
    bool partial = false;
    uint32_t output_index = g_pool.next;
    if (raw) {
        // Slot 0 is the persistent packed-gray accumulator for vendor output.
        // A new helper/connection, geometry change, or onebit change forces a
        // full frame before any partial update is allowed.
        output_index = 0;
        partial = useful_partial_damage(request) && g_accum_valid &&
                  g_accum_width == request.width && g_accum_height == request.height &&
                  g_accum_onebit == request.onebit;
    } else {
        g_accum_valid = false;
    }
    if (!partial) {
        effective.left = effective.top = effective.right = effective.bottom = UINT32_MAX;
    }
    if (g_converter_mode != ConverterMode::CPU) {
        AHardwareBuffer* output = g_pool.buffers[output_index];
        if (try_gpu_convert(input, output_index, effective)) {
            if (raw_target.value >= 0) {
                void* packed = nullptr;
                result = AHardwareBuffer_lock(output, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1,
                                              nullptr, &packed);
                if (result != 0 || !packed) {
                    return request_failure(client, STATUS_LOCK_OUTPUT,
                                           result != 0 ? result : EINVAL,
                                           "GPU output lock for raw copy failed");
                }
                GrayStatus raw_status = STATUS_RAW_COPY;
                const int raw_error = copy_packed_to_raw_fd(
                    static_cast<const uint8_t*>(packed), raw_target.value, gray_bytes,
                    effective, partial, &raw_status);
                const int unlock_result = AHardwareBuffer_unlock(output, nullptr);
                raw_target.reset();
                if (raw_error != 0) {
                    return request_failure(client, raw_status, raw_error,
                                           "raw output-target copy failed");
                }
                if (unlock_result != 0) {
                    return request_failure(client, STATUS_UNLOCK_OUTPUT, unlock_result,
                                           "GPU output unlock after raw copy failed");
                }
            }

            result = AHardwareBuffer_sendHandleToUnixSocket(output, client);
            if (result != 0) {
                return request_failure(client, STATUS_SEND_OUTPUT, result,
                                       "GPU output send failed");
            }
            if (raw) {
                g_accum_valid = true;
                g_accum_width = request.width;
                g_accum_height = request.height;
                g_accum_onebit = (uint8_t)request.onebit;
                if (partial) {
                    static uint32_t logged = 0;
                    if (logged++ < 4)
                        LOGI("partial gray update (%u,%u)-(%u,%u) %ux%u",
                             effective.left, effective.top, effective.right, effective.bottom,
                             request.width, request.height);
                }
            } else {
                g_pool.next = (output_index + 1) % kPoolSize;
            }
            return 0;
        }
        LOGD("GPU converter unavailable; falling back to CPU NEON");
    }

    void* input_map = nullptr;
    const uint64_t input_read_usage =
        (input_desc.usage & AHARDWAREBUFFER_USAGE_CPU_READ_RARELY)
            ? AHARDWAREBUFFER_USAGE_CPU_READ_RARELY
            : AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
    result = AHardwareBuffer_lock(input, input_read_usage, -1, nullptr, &input_map);
    if (result != 0 || !input_map) {
        return request_failure(client, STATUS_LOCK_INPUT, result != 0 ? result : EINVAL,
                               "input lock failed");
    }

    AHardwareBuffer* output = g_pool.buffers[output_index];
    void* output_map = nullptr;
    result = AHardwareBuffer_lock(output, AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY, -1, nullptr,
                                  &output_map);
    if (result != 0 || !output_map) {
        const int saved = result != 0 ? result : EINVAL;
        AHardwareBuffer_unlock(input, nullptr);
        return request_failure(client, STATUS_LOCK_OUTPUT, saved, "output lock failed");
    }

    AHardwareBuffer_Desc output_desc{};
    AHardwareBuffer_describe(output, &output_desc);
    if (!valid_rgba_backing(output_desc, request.width, request.height)) {
        AHardwareBuffer_unlock(output, nullptr);
        AHardwareBuffer_unlock(input, nullptr);
        return request_failure(client, STATUS_ALLOCATE, EINVAL,
                               "output RGBA backing is invalid");
    }
    const size_t input_pitch = static_cast<size_t>(input_desc.stride) * 4;
    // The e-ink commit reads this RGBA allocation as packed gray8: exactly
    // width*height contiguous bytes at offset 0, with logical pitch = width.
    const size_t output_pitch = request.width;
    gray_convert(static_cast<uint8_t*>(output_map), static_cast<const uint8_t*>(input_map),
                 request.width, request.height, input_pitch, output_pitch, request.onebit);

    if (raw_target.value >= 0) {
        GrayStatus raw_status = STATUS_RAW_COPY;
        const int raw_error = copy_packed_to_raw_fd(
            static_cast<const uint8_t*>(output_map), raw_target.value, gray_bytes,
            effective, partial, &raw_status);
        raw_target.reset();
        if (raw_error != 0) {
            AHardwareBuffer_unlock(output, nullptr);
            AHardwareBuffer_unlock(input, nullptr);
            return request_failure(client, raw_status, raw_error,
                                   "raw output-target copy failed");
        }
    }

    const int output_unlock = AHardwareBuffer_unlock(output, nullptr);
    const int input_unlock = AHardwareBuffer_unlock(input, nullptr);

    if (output_unlock != 0) {
        return request_failure(client, STATUS_UNLOCK_OUTPUT, output_unlock,
                               "output unlock failed");
    }
    if (input_unlock != 0) {
        return request_failure(client, STATUS_UNLOCK_INPUT, input_unlock,
                               "input unlock failed");
    }

    result = AHardwareBuffer_sendHandleToUnixSocket(output, client);
    if (result != 0) {
        return request_failure(client, STATUS_SEND_OUTPUT, result, "output send failed");
    }

    if (raw) {
        g_accum_valid = true;
        g_accum_width = request.width;
        g_accum_height = request.height;
        g_accum_onebit = (uint8_t)request.onebit;
    } else {
        g_pool.next = (output_index + 1) % kPoolSize;
    }
    LOGD("converted %ux%u onebit=%u partial=%d input_stride=%u output_pitch=%u backing_stride=%u",
         request.width, request.height, request.onebit, partial ? 1u : 0u,
         input_desc.stride, request.width, output_desc.stride);
    return 0;
}

int handle_client(int client) {
    GrayRequest request{};
    const ssize_t received = recv(client, &request, sizeof(request), MSG_TRUNC);
    if (received != static_cast<ssize_t>(sizeof(request))) {
        if (received < 0) {
            return request_failure(client, STATUS_IO, errno, "request receive failed");
        }
        return request_failure(client, STATUS_PROTOCOL, EMSGSIZE, "bad request packet size");
    }
    if (request.magic != kRequestMagic || request.version != kProtocolVersion ||
        request.onebit > 1 || request.reserved > 1 ||
        !valid_geometry(request.width, request.height)) {
        return request_failure(client, STATUS_PROTOCOL, EINVAL, "invalid GrayRequest");
    }
    const bool full_damage = request.left == UINT32_MAX && request.top == UINT32_MAX &&
                             request.right == UINT32_MAX && request.bottom == UINT32_MAX;
    const bool any_damage = request.left != UINT32_MAX || request.top != UINT32_MAX ||
                            request.right != UINT32_MAX || request.bottom != UINT32_MAX;
    uint32_t left, top, right, bottom;
    if ((!full_damage && !any_damage) ||
        (any_damage && (request.reserved != 1 ||
                       !request_damage(request, &left, &top, &right, &bottom)))) {
        return request_failure(client, STATUS_PROTOCOL, EINVAL, "invalid damage rectangle");
    }
    return process_request(client, request);
}

int set_client_options(int client) {
    const int flags = fcntl(client, F_GETFD);
    if (flags < 0 || fcntl(client, F_SETFD, flags | FD_CLOEXEC) != 0) {
        LOGE("client FD_CLOEXEC failed: %s", strerror(errno));
        return -1;
    }

    const timeval timeout{kSocketTimeoutSeconds, 0};
    if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        LOGE("client socket timeout failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int prepare_socket_path(const char* path) {
    struct stat info{};
    if (lstat(path, &info) != 0) {
        return errno == ENOENT ? 0 : -errno;
    }
    if (!S_ISSOCK(info.st_mode)) {
        errno = EEXIST;
        return -1;
    }

    // Do not steal the path from a live helper. A refused probe denotes a
    // stale socket, which is safe to remove before bind().
    int probe = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (probe < 0) return -errno;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    if (connect(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
        close(probe);
        errno = EADDRINUSE;
        return -1;
    }
    const int probe_error = errno;
    close(probe);
    if (probe_error != ECONNREFUSED && probe_error != ENOENT) {
        errno = probe_error;
        return -1;
    }
    return unlink(path) == 0 ? 0 : -errno;
}

int create_listener(const char* path) {
    if (strlen(path) == 0 || strlen(path) >= sizeof(sockaddr_un::sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    const int path_result = prepare_socket_path(path);
    if (path_result != 0) {
        LOGE("socket path %s is unavailable: %s", path, strerror(-path_result));
        return -1;
    }

    int listener = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (listener < 0) return -1;
    const int flags = fcntl(listener, F_GETFD);
    if (flags >= 0) fcntl(listener, F_SETFD, flags | FD_CLOEXEC);

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        const int saved = errno;
        close(listener);
        errno = saved;
        return -1;
    }

    // The supported deployment uses the abstract reverse socket. Keep the
    // standalone filesystem test socket root-only instead of world-writable.
    if (chmod(path, 0600) != 0 || listen(listener, 4) != 0) {
        const int saved = errno;
        close(listener);
        unlink(path);
        errno = saved;
        return -1;
    }
    return listener;
}

volatile sig_atomic_t g_stop = 0;

void stop_handler(int) {
    g_stop = 1;
}

bool valid_abstract_name(const char* name) {
    if (!name || name[0] == '\0' || name[0] == '/') return false;
    return strlen(name) < sizeof(sockaddr_un::sun_path);
}

int connect_abstract_socket(const char* name) {
    const size_t name_length = strlen(name);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    address.sun_path[0] = '\0';
    memcpy(address.sun_path + 1, name, name_length);
    const socklen_t address_length = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + 1 + name_length);

    const int client = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (client < 0) return -1;
    const int flags = fcntl(client, F_GETFD);
    if (flags >= 0) fcntl(client, F_SETFD, flags | FD_CLOEXEC);
    if (connect(client, reinterpret_cast<sockaddr*>(&address), address_length) == 0) {
        return client;
    }
    const int saved = errno;
    close(client);
    errno = saved;
    return -1;
}

int run_connect_mode(const char* name, bool once) {
    LOGI("gray_helper connecting to abstract socket %s (%s)", name, once ? "once" : "daemon");
    // First-use PowerVR/EGL and gralloc allocation are much slower than a warm
    // frame. Do them before the first request so the 150 ms HWC budget is not
    // spent on initialization.
    if (ensure_egl() == 0) {
        const int warm = ensure_pool(1440, 720);
        LOGI("gray_helper warmup: egl=ok pool=%d", warm);
    } else {
        LOGE("gray_helper warmup: EGL initialization failed");
    }
    bool waiting_logged = false;
    uint32_t retry_count = 0;
    while (!g_stop) {
        const int client = connect_abstract_socket(name);
        if (client < 0) {
            if (!waiting_logged) {
                LOGI("waiting for abstract socket %s: %s", name, strerror(errno));
                waiting_logged = true;
            } else if (++retry_count % 40 == 0) {
                LOGD("still waiting for abstract socket %s: %s", name, strerror(errno));
            }
            usleep(kConnectRetryMicros);
            continue;
        }

        waiting_logged = false;
        // A reconnect may follow a failed/ambiguous partial update. Force the
        // first frame on the new connection to rebuild the accumulator.
        g_accum_valid = false;
        LOGI("connected to abstract socket %s", name);
        if (set_client_options(client) != 0) {
            close(client);
            if (once) return 1;
            continue;
        }
        // One connection can carry one or more exchanges until the client closes.
        while (!g_stop) {
            GrayRequest peek{};
            const ssize_t peeked = recv(client, &peek, sizeof(peek), MSG_PEEK);
            if (peeked == 0) break;
            if (peeked < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                if (errno == ECONNRESET || errno == ENOTCONN) break;
                LOGE("request peek failed: %s", strerror(errno));
                break;
            }
            const int client_result = handle_client(client);
            if (once) {
                close(client);
                return client_result;
            }
            if (client_result != 0) break;
        }
        close(client);
        if (once) return 0;
    }
    return 0;
}

void usage(const char* argv0) {
    printf("usage: %s [--once] [--cpu] [--socket PATH | --connect NAME]\n"
           "  default filesystem socket: %s\n"
           "  --connect NAME  repeatedly connect to abstract AF_UNIX socket NAME\n"
           "  --cpu           force CPU NEON instead of the default GPU path\n"
           "  --once          handle one connection, then exit\n",
           argv0, kDefaultSocket);
}

}  // namespace

int main(int argc, char** argv) {
    const char* socket_path = kDefaultSocket;
    const char* connect_name = nullptr;
    bool socket_option_seen = false;
    bool once = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--once") == 0) {
            once = true;
        } else if (strcmp(argv[i], "--cpu") == 0) {
            g_converter_mode = ConverterMode::CPU;
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            if (connect_name || socket_option_seen) {
                usage(argv[0]);
                return 2;
            }
            socket_option_seen = true;
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "--connect") == 0 && i + 1 < argc) {
            if (connect_name || socket_option_seen) {
                usage(argv[0]);
                return 2;
            }
            connect_name = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (connect_name && !valid_abstract_name(connect_name)) {
        fprintf(stderr, "--connect requires a non-empty abstract name without a leading '/'\n");
        usage(argv[0]);
        return 2;
    }

    struct sigaction action{};
    action.sa_handler = stop_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    signal(SIGPIPE, SIG_IGN);

    int exit_code = 0;
    if (connect_name) {
        exit_code = run_connect_mode(connect_name, once);
    } else {
        const int listener = create_listener(socket_path);
        if (listener < 0) {
            LOGE("listen on %s failed: %s", socket_path, strerror(errno));
            return 1;
        }
        LOGI("gray_helper listening on %s (%s)", socket_path, once ? "once" : "daemon");

        while (!g_stop) {
            const int client = accept(listener, nullptr, nullptr);
            if (client < 0) {
                if (errno == EINTR) continue;
                LOGE("accept failed: %s", strerror(errno));
                exit_code = 1;
                break;
            }
            if (set_client_options(client) != 0) {
                close(client);
                if (once) { exit_code = 1; break; }
                continue;
            }
            const int client_result = handle_client(client);
            close(client);
            if (once) {
                exit_code = client_result;
                break;
            }
            // A malformed or failed client must not terminate the daemon.
        }

        close(listener);
        unlink(socket_path);
    }

    release_input_cache();
    release_pool();
    gpu_cleanup();
    egl_cleanup();
    LOGI("gray_helper stopped");
    return exit_code;
}
