/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * dmabuf_relay.c — the sandboxed side, with nothing in it.
 *
 * Receives a dma-buf fd plus its descriptor from `dmabuf_source --serve`
 * (which stands in for the isolate) and relays it to the broker with
 * ATTACH + COMMIT.  That is precisely what nvkvm_display_relay.c does inside
 * QEMU: the buffer arrives as an fd over a socket and is passed on as an fd
 * over another socket.  Nothing is imported, mapped, or interpreted.
 *
 * THE POINT IS THE LINK LINE.  This binary links libc and nothing else: no
 * GBM, no EGL, no GL, no libdrm, no X11, no Wayland.  Run `ldd` on it inside
 * the container.  If the display-only container profile is real, a process
 * shaped like this is all the container needs to hold.
 *
 *     dmabuf_relay --from /run/nvkvm/src.sock --broker /run/nvkvm/display.sock
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/un.h>

#include "../common/nvkvm_broker_proto.h"

struct src_desc {
    uint32_t width, height, stride, offset, fourcc;
    uint64_t modifier;
    uint64_t size;
};

static int connect_unix(const char *path)
{
    struct sockaddr_un sa;
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (s < 0) {
        return -1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

static int recv_fd(int sock, void *buf, size_t len, int *fd_out)
{
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { .iov_base = buf, .iov_len = len };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
    struct cmsghdr *cm;
    ssize_t n;

    memset(cbuf, 0, sizeof(cbuf));
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);
    n = recvmsg(sock, &msg, 0);
    if (n != (ssize_t)len) {
        return -1;
    }
    for (cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
            memcpy(fd_out, CMSG_DATA(cm), sizeof(int));
            return 0;
        }
    }
    return -1;
}

static int send_cmd(int sock, const struct nvkvm_broker_cmd *c, int fd)
{
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { .iov_base = (void *)c, .iov_len = sizeof(*c) };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
    struct cmsghdr *cm;

    if (fd >= 0) {
        memset(cbuf, 0, sizeof(cbuf));
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof(cbuf);
        cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    }
    return sendmsg(sock, &msg, 0) < 0 ? -1 : 0;
}

int main(int argc, char **argv)
{
    const char *from = NULL, *broker = NULL;
    int src, bs, dmafd = -1, i;
    unsigned frames = 8, f;
    struct src_desc d;
    struct nvkvm_broker_pkt p;
    struct nvkvm_broker_cmd c;

    setvbuf(stdout, NULL, _IOLBF, 0);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (!strcmp(a, "--from") && v)        { from = v; i++; }
        else if (!strcmp(a, "--broker") && v) { broker = v; i++; }
        else if (!strcmp(a, "--frames") && v) { frames = (unsigned)atoi(v); i++; }
        else {
            fprintf(stderr, "usage: %s --from SOCK --broker SOCK "
                    "[--frames N]\n", argv[0]);
            return 2;
        }
    }
    if (!from || !broker) {
        fprintf(stderr, "both --from and --broker are required\n");
        return 2;
    }

    src = connect_unix(from);
    if (src < 0) {
        fprintf(stderr, "connect %s: %s\n", from, strerror(errno));
        return 1;
    }
    if (recv_fd(src, &d, sizeof(d), &dmafd) < 0) {
        fprintf(stderr, "no fd from %s\n", from);
        return 1;
    }
    printf("received a dma-buf fd: %ux%u stride=%u offset=%u "
           "modifier=0x%016llx size=%llu\n", d.width, d.height, d.stride,
           d.offset, (unsigned long long)d.modifier,
           (unsigned long long)d.size);
    printf("this process has never opened a render node and links no "
           "graphics library.\n");

    bs = connect_unix(broker);
    if (bs < 0) {
        fprintf(stderr, "connect %s: %s\n", broker, strerror(errno));
        return 1;
    }
    if (read(bs, &p, sizeof(p)) != (ssize_t)sizeof(p) ||
        p.type != NVKVM_BROKER_EV_HELLO) {
        fprintf(stderr, "no HELLO from the broker\n");
        return 1;
    }
    printf("HELLO: proto %u caps 0x%04x\n", p.w0, p.w1);

    for (f = 0; f < frames; f++) {
        memset(&c, 0, sizeof(c));
        c.type = NVKVM_BROKER_CMD_ATTACH;
        c.width = d.width; c.height = d.height;
        c.stride = d.stride; c.offset = d.offset;
        c.fourcc = d.fourcc; c.modifier = d.modifier;
        c.seq = f;
        if (send_cmd(bs, &c, dmafd) < 0) {
            fprintf(stderr, "ATTACH: %s\n", strerror(errno));
            return 1;
        }
        memset(&c, 0, sizeof(c));
        c.type = NVKVM_BROKER_CMD_COMMIT;
        c.seq = f;
        if (send_cmd(bs, &c, -1) < 0) {
            fprintf(stderr, "COMMIT: %s\n", strerror(errno));
            return 1;
        }
        printf("relayed frame %u\n", f);
        usleep(150000);
    }
    printf("done; any rejection appears on the BROKER's stderr.\n");
    for (;;) {
        ssize_t n = read(bs, &p, sizeof(p));

        if (n <= 0) {
            printf("broker closed the connection\n");
            break;
        }
        if (p.type == NVKVM_BROKER_EV_BYE) {
            printf("BYE reason %d\n", p.x);
            break;
        }
    }
    return 0;
}
