#include <android/hardware_buffer.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main() {
    AHardwareBuffer_Desc d{};
    d.width = 1440; d.height = 720; d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    d.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER |
              AHARDWAREBUFFER_USAGE_CPU_READ_RARELY | AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY;
    AHardwareBuffer* b = nullptr;
    int r = AHardwareBuffer_allocate(&d, &b);
    if (r != 0 || !b) { printf("alloc=%d\n", r); return 1; }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) return 1;
    r = AHardwareBuffer_sendHandleToUnixSocket(b, sv[0]);
    printf("send=%d\n", r);
    int32_t words[4096];
    char control[CMSG_SPACE(128 * sizeof(int))];
    memset(control, 0, sizeof(control));
    iovec iov{words, sizeof(words)};
    msghdr m{}; m.msg_iov = &iov; m.msg_iovlen = 1; m.msg_control = control; m.msg_controllen = sizeof(control);
    ssize_t n = recvmsg(sv[1], &m, 0);
    printf("recv=%zd controllen=%zu flags=%x\n", n, m.msg_controllen, m.msg_flags);
    for (int i = 0; i < 13 && i < (int)(n / 4); ++i) printf("w[%d]=%08x (%d)\n", i, (uint32_t)words[i], words[i]);
    cmsghdr* c = CMSG_FIRSTHDR(&m);
    if (c) {
        int count = (int)(c->cmsg_len - CMSG_LEN(0)) / (int)sizeof(int);
        int* fds = (int*)CMSG_DATA(c);
        printf("fds=%d", count);
        for (int i = 0; i < count; ++i) printf(" %d", fds[i]);
        printf("\n");
        for (int i = 0; i < count; ++i) close(fds[i]);
    }
    close(sv[0]); close(sv[1]); AHardwareBuffer_release(b);
    return 0;
}
