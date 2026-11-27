// gray_helper_client -- protocol, conversion, and latency test for gray_helper.
// This is a standalone /data/local/tmp test process; it does not mount or patch
// SurfaceFlinger, HWC, gralloc, or any partition.

#include <android/hardware_buffer.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

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
static_assert(sizeof(GrayRequest) == 40, "request wire size");

static constexpr uint32_t kRequestMagic = 0x31475748u; // HWG1
static constexpr uint32_t kErrorMagic = 0x31455748u;   // HWE1
static constexpr uint32_t kRawTargetMagic = 0x314f5748u; // HWO1

static const uint8_t kBayer4[4][16] = {
    { 0, 8, 2,10, 0, 8, 2,10, 0, 8, 2,10, 0, 8, 2,10 },
    {12, 4,14, 6,12, 4,14, 6,12, 4,14, 6,12, 4,14, 6 },
    { 3,11, 1, 9, 3,11, 1, 9, 3,11, 1, 9, 3,11, 1, 9 },
    {15, 7,13, 5,15, 7,13, 5,15, 7,13, 5,15, 7,13, 5 },
};
static const uint8_t kBayer8[8][8] = {
    { 0,32, 8,40, 2,34,10,42 },
    {48,16,56,24,50,18,58,26 },
    {12,44, 4,36,14,46, 6,38 },
    {60,28,52,20,62,30,54,22 },
    { 3,35,11,43, 1,33, 9,41 },
    {51,19,59,27,49,17,57,25 },
    {15,47, 7,39,13,45, 5,37 },
    {63,31,55,23,61,29,53,21 },
};

static uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int egl_setup() {
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) return -1;
    EGLint major = 0, minor = 0;
    if (!eglInitialize(display, &major, &minor)) return -1;
    const EGLint config_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint count = 0;
    if (!eglChooseConfig(display, config_attrs, &config, 1, &count) || count < 1) return -1;
    const EGLint pbuffer_attrs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, pbuffer_attrs);
    if (surface == EGL_NO_SURFACE) return -1;
    if (!eglBindAPI(EGL_OPENGL_ES_API)) return -1;
    const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    if (context == EGL_NO_CONTEXT) return -1;
    if (!eglMakeCurrent(display, surface, surface, context)) return -1;
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    printf("EGL %d.%d renderer=%s\n", major, minor, renderer ? renderer : "unknown");
    // Intentionally leave the context current for process lifetime.
    return 0;
}

static int connect_helper(const char* path) {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    timeval timeout{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    return fd;
}

static int create_abstract_listener(const char* name) {
    if (!name || !name[0] || name[0] == '/') return -1;
    const size_t length = strlen(name);
    if (length >= sizeof(sockaddr_un::sun_path) - 1) return -1;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path + 1, name, length);
    const socklen_t address_length = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + 1 + length);
    if (bind(fd, (sockaddr*)&addr, address_length) != 0 || listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int accept_helper(int listener) {
    int fd = accept(listener, nullptr, nullptr);
    if (fd < 0) return -1;
    timeval timeout{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    return fd;
}

static int create_raw_target(size_t bytes) {
#ifdef SYS_memfd_create
    int fd = static_cast<int>(syscall(SYS_memfd_create, "gray-helper-raw-test", 1U));
#else
    int fd = -1;
#endif
    if (fd < 0) return -1;
    if (ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_raw_target(int client, int raw_fd) {
    uint32_t payload = kRawTargetMagic;
    struct iovec iov {&payload, sizeof(payload)};
    alignas(struct cmsghdr) uint8_t control[CMSG_SPACE(sizeof(int))]{};
    struct msghdr message{};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &raw_fd, sizeof(raw_fd));
    return sendmsg(client, &message, MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(payload)) ? 0 : -1;
}

static uint8_t expected_pixel(uint32_t x, uint32_t y, uint32_t onebit) {
    const uint32_t r = (x * 7u + y * 3u) & 255u;
    const uint32_t g = (x * 13u + y * 5u) & 255u;
    const uint32_t b = (x * 17u + y * 11u) & 255u;
    const uint32_t luma = (77u * r + 150u * g + 29u * b) >> 8;
    if (onebit) return (luma + kBayer8[y & 7][x & 7] >= 128) ? 255 : 0;
    uint32_t q = (luma + kBayer4[y & 3][x & 15]) >> 4;
    if (q > 15) q = 15;
    return (uint8_t)(q * 17u);
}

static int exchange(int client, AHardwareBuffer* src, int raw_fd,
                    const GrayRequest& request, AHardwareBuffer** output) {
    *output = nullptr;
    if (send(client, &request, sizeof(request), MSG_NOSIGNAL) != (ssize_t)sizeof(request) ||
        AHardwareBuffer_sendHandleToUnixSocket(src, client) != 0 ||
        send_raw_target(client, raw_fd) != 0) return -1;
    uint32_t magic = 0;
    if (recv(client, &magic, sizeof(magic), MSG_PEEK) == (ssize_t)sizeof(magic) &&
        magic == kErrorMagic) {
        uint32_t error[6]{};
        recv(client, error, sizeof(error), 0);
        fprintf(stderr, "helper error status=%d native=%d\n",
                (int32_t)error[2], (int32_t)error[3]);
        return -1;
    }
    return AHardwareBuffer_recvHandleFromUnixSocket(client, output);
}

static int change_rect(AHardwareBuffer* src, uint32_t left, uint32_t top,
                       uint32_t right, uint32_t bottom) {
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(src, &desc);
    void* map = nullptr;
    int rc = AHardwareBuffer_lock(src, AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY, -1,
                                  nullptr, &map);
    if (rc != 0 || !map) return rc != 0 ? rc : EINVAL;
    for (uint32_t y = top; y <= bottom; ++y) {
        uint32_t* row = (uint32_t*)((uint8_t*)map + (size_t)y * desc.stride * 4);
        for (uint32_t x = left; x <= right; ++x) {
            row[x] = ((x * 7u + y * 3u + 31u) & 255u) |
                     (((x * 13u + y * 5u + 67u) & 255u) << 8) |
                     (((x * 17u + y * 11u + 113u) & 255u) << 16) | 0xff000000u;
        }
    }
    return AHardwareBuffer_unlock(src, nullptr);
}

int main(int argc, char** argv) {
    const char* path = "/data/local/tmp/hwcgray.sock";
    const char* listen_name = nullptr;
    uint32_t width = 1440, height = 720, iters = 12;
    bool partial_test = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc) path = argv[++i];
        else if (!strcmp(argv[i], "--listen") && i + 1 < argc) listen_name = argv[++i];
        else if (!strcmp(argv[i], "--w") && i + 1 < argc) width = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--h") && i + 1 < argc) height = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--partial")) partial_test = true;
        else {
            fprintf(stderr, "usage: %s [--socket PATH | --listen NAME] [--w N] [--h N] [--iters N] [--partial]\n", argv[0]);
            return 2;
        }
    }
    if (!width || !height || !iters) return 2;

    AHardwareBuffer_Desc src_desc{};
    src_desc.width = width;
    src_desc.height = height;
    src_desc.layers = 1;
    src_desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    src_desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                     AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
                     AHARDWAREBUFFER_USAGE_CPU_READ_RARELY |
                     AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY;
    AHardwareBuffer* src = nullptr;
    int rc = AHardwareBuffer_allocate(&src_desc, &src);
    if (rc != 0 || !src) {
        fprintf(stderr, "source allocation failed: %d\n", rc);
        return 1;
    }
    AHardwareBuffer_Desc src_got{};
    AHardwareBuffer_describe(src, &src_got);
    void* src_map = nullptr;
    rc = AHardwareBuffer_lock(src, AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY, -1, nullptr, &src_map);
    if (rc != 0 || !src_map) {
        fprintf(stderr, "source lock failed: %d\n", rc);
        return 1;
    }
    for (uint32_t y = 0; y < height; ++y) {
        uint32_t* row = (uint32_t*)((uint8_t*)src_map + (size_t)y * src_got.stride * 4);
        for (uint32_t x = 0; x < width; ++x) {
            row[x] = ((x * 7u + y * 3u) & 255u) |
                     (((x * 13u + y * 5u) & 255u) << 8) |
                     (((x * 17u + y * 11u) & 255u) << 16) | 0xff000000u;
        }
    }
    rc = AHardwareBuffer_unlock(src, nullptr);
    if (rc != 0) {
        fprintf(stderr, "source unlock failed: %d\n", rc);
        return 1;
    }
    printf("source %ux%u stride=%u\n", width, height, src_got.stride);

    if (egl_setup() != 0) {
        fprintf(stderr, "EGL setup failed: 0x%x\n", eglGetError());
        return 1;
    }

    int listener = -1;
    if (listen_name) {
        listener = create_abstract_listener(listen_name);
        if (listener < 0) {
            fprintf(stderr, "abstract listener %s failed: %s\n", listen_name, strerror(errno));
            return 1;
        }
        printf("abstract listener ready: %s\n", listen_name);
    }

    if (partial_test) {
        int failed = 0;
        const uint32_t left = 3;
        const uint32_t top = 5;
        const uint32_t right = width > 8 ? (width * 2 / 3) : width - 1;
        const uint32_t bottom = height > 8 ? (height / 3) : height - 1;
        const size_t gray_bytes = (size_t)width * height;
        for (uint32_t onebit = 0; onebit <= 1 && !failed; ++onebit) {
            int fd = listener >= 0 ? accept_helper(listener) : connect_helper(path);
            int raw_full = fd >= 0 ? create_raw_target(gray_bytes) : -1;
            AHardwareBuffer* out = nullptr;
            GrayRequest full{kRequestMagic, 2, width, height, onebit, 1,
                             UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
            if (fd < 0 || raw_full < 0 || exchange(fd, src, raw_full, full, &out) != 0 || !out) {
                fprintf(stderr, "partial setup full frame failed\n");
                failed = 1;
            }
            if (out) AHardwareBuffer_release(out);
            if (raw_full >= 0) close(raw_full);

            int raw_partial = -1;
            if (!failed && change_rect(src, left, top, right, bottom) != 0) {
                fprintf(stderr, "source rectangle update failed\n");
                failed = 1;
            }
            if (!failed) usleep(20000);
            if (!failed) {
                GrayRequest part{kRequestMagic, 2, width, height, onebit, 1,
                                 left, top, right, bottom};
                raw_partial = create_raw_target(gray_bytes);
                out = nullptr;
                if (raw_partial < 0 || exchange(fd, src, raw_partial, part, &out) != 0 || !out) {
                    fprintf(stderr, "partial frame exchange failed\n");
                    failed = 1;
                }
                if (out) AHardwareBuffer_release(out);
            }
            if (fd >= 0) close(fd);

            int raw_reference = -1;
            if (!failed) {
                fd = listener >= 0 ? accept_helper(listener) : connect_helper(path);
                raw_reference = fd >= 0 ? create_raw_target(gray_bytes) : -1;
                out = nullptr;
                if (fd < 0 || raw_reference < 0 ||
                    exchange(fd, src, raw_reference, full, &out) != 0 || !out) {
                    fprintf(stderr, "partial reference full frame failed\n");
                    failed = 1;
                }
                if (out) AHardwareBuffer_release(out);
                if (fd >= 0) close(fd);
            }

            if (!failed) {
                void* a = mmap(nullptr, gray_bytes, PROT_READ, MAP_SHARED, raw_partial, 0);
                void* b = mmap(nullptr, gray_bytes, PROT_READ, MAP_SHARED, raw_reference, 0);
                if (a == MAP_FAILED || b == MAP_FAILED) {
                    fprintf(stderr, "partial compare mmap failed\n");
                    failed = 1;
                } else {
                    const uint8_t* pa = (const uint8_t*)a;
                    const uint8_t* pb = (const uint8_t*)b;
                    uint64_t mismatches = 0;
                    size_t first = 0;
                    for (size_t i = 0; i < gray_bytes; ++i) {
                        if (pa[i] != pb[i]) {
                            if (!mismatches) first = i;
                            ++mismatches;
                        }
                    }
                    printf("partial onebit=%u rect=(%u,%u)-(%u,%u) mismatches=%llu first=%zu\n",
                           onebit, left, top, right, bottom,
                           (unsigned long long)mismatches, first);
                    if (mismatches) failed = 1;
                }
                if (a != MAP_FAILED) munmap(a, gray_bytes);
                if (b != MAP_FAILED) munmap(b, gray_bytes);
            }
            if (raw_partial >= 0) close(raw_partial);
            if (raw_reference >= 0) close(raw_reference);
        }
        if (listener >= 0) close(listener);
        AHardwareBuffer_release(src);
        printf("%s\n", failed ? "FAIL" : "PASS");
        return failed;
    }

    int failed = 0;
    for (uint32_t onebit = 0; onebit <= 1 && !failed; ++onebit) {
        double sum_ms = 0, max_ms = 0;
        for (uint32_t n = 0; n < iters; ++n) {
            const uint64_t started = now_ns();
            int fd = listener >= 0 ? accept_helper(listener) : connect_helper(path);
            if (fd < 0) {
                fprintf(stderr, "%s failed: %s\n", listener >= 0 ? "accept" : "connect", strerror(errno));
                failed = 1;
                break;
            }
            const size_t gray_bytes = static_cast<size_t>(width) * height;
            int raw_fd = create_raw_target(gray_bytes);
            if (raw_fd < 0) {
                fprintf(stderr, "raw target creation failed: %s\n", strerror(errno));
                close(fd);
                failed = 1;
                break;
            }
            GrayRequest request{kRequestMagic, 2, width, height, onebit, 1,
                                UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
            if (send(fd, &request, sizeof(request), MSG_NOSIGNAL) != (ssize_t)sizeof(request) ||
                AHardwareBuffer_sendHandleToUnixSocket(src, fd) != 0 ||
                send_raw_target(fd, raw_fd) != 0) {
                fprintf(stderr, "request send failed: %s\n", strerror(errno));
                close(raw_fd);
                close(fd);
                failed = 1;
                break;
            }
            uint32_t magic = 0;
            if (recv(fd, &magic, sizeof(magic), MSG_PEEK) == (ssize_t)sizeof(magic) &&
                magic == kErrorMagic) {
                uint32_t error[6]{};
                recv(fd, error, sizeof(error), 0);
                fprintf(stderr, "helper error status=%d native=%d\n",
                        (int32_t)error[2], (int32_t)error[3]);
                close(raw_fd);
                close(fd);
                failed = 1;
                break;
            }
            AHardwareBuffer* out = nullptr;
            rc = AHardwareBuffer_recvHandleFromUnixSocket(fd, &out);
            close(fd);
            if (rc != 0 || !out) {
                fprintf(stderr, "output receive failed: %d\n", rc);
                close(raw_fd);
                failed = 1;
                break;
            }
            AHardwareBuffer_Desc desc{};
            AHardwareBuffer_describe(out, &desc);
            // The helper intentionally uses an RGBA8888 gralloc backing because
            // this allocator rejects R8. Only the first packed width*height bytes
            // are gray8; the e-ink ioctl uses format=0 and stride=width.
            if (desc.format != AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM || desc.width != width ||
                desc.height != height || desc.layers != 1 || desc.stride < width) {
                fprintf(stderr, "bad output format=0x%x %ux%u layers=%u stride=%u\n",
                        desc.format, desc.width, desc.height, desc.layers, desc.stride);
                AHardwareBuffer_release(out);
                close(raw_fd);
                failed = 1;
                break;
            }
            void* map = nullptr;
            rc = AHardwareBuffer_lock(out, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, nullptr, &map);
            if (rc != 0 || !map) {
                fprintf(stderr, "output lock failed: %d\n", rc);
                AHardwareBuffer_release(out);
                close(raw_fd);
                failed = 1;
                break;
            }
            void* raw_map = mmap(nullptr, gray_bytes, PROT_READ, MAP_SHARED, raw_fd, 0);
            if (raw_map == MAP_FAILED) {
                fprintf(stderr, "raw target mmap failed: %s\n", strerror(errno));
                AHardwareBuffer_unlock(out, nullptr);
                AHardwareBuffer_release(out);
                close(raw_fd);
                failed = 1;
                break;
            }
            uint64_t mismatches = 0;
            uint64_t raw_mismatches = 0;
            uint32_t first_x = 0, first_y = 0;
            uint8_t first_got = 0;
            for (uint32_t y = 0; y < height; ++y) {
                const uint8_t* row = (const uint8_t*)map + (size_t)y * width;
                const uint8_t* raw_row = (const uint8_t*)raw_map + (size_t)y * width;
                for (uint32_t x = 0; x < width; ++x) {
                    const uint8_t expected = expected_pixel(x, y, onebit);
                    if (row[x] != expected) {
                        if (!mismatches) { first_x = x; first_y = y; first_got = row[x]; }
                        ++mismatches;
                    }
                    if (raw_row[x] != expected) ++raw_mismatches;
                }
            }
            munmap(raw_map, gray_bytes);
            close(raw_fd);
            AHardwareBuffer_unlock(out, nullptr);
            AHardwareBuffer_release(out);
            const double elapsed = (double)(now_ns() - started) / 1e6;
            if (n > 0) { sum_ms += elapsed; if (elapsed > max_ms) max_ms = elapsed; }
            printf("onebit=%u frame=%u backing_stride=%u gray_pitch=%u total=%.2fms mismatches=%llu raw_mismatches=%llu",
                   onebit, n, desc.stride, width, elapsed,
                   (unsigned long long)mismatches, (unsigned long long)raw_mismatches);
            if (mismatches || raw_mismatches) {
                printf(" first=(%u,%u) got=%u expected=%u", first_x, first_y,
                       first_got,
                       expected_pixel(first_x, first_y, onebit));
                failed = 1;
            }
            printf("\n");
            if (mismatches || raw_mismatches) break;
        }
        if (!failed && iters > 1)
            printf("onebit=%u warm average=%.2fms max=%.2fms\n",
                   onebit, sum_ms / (double)(iters - 1), max_ms);
    }

    if (listener >= 0) close(listener);
    AHardwareBuffer_release(src);
    printf("%s\n", failed ? "FAIL" : "PASS");
    return failed;
}
