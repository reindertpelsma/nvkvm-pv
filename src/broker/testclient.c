/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * testclient.c — connect to the broker, print every event, and (optionally)
 * send buffers at it, including deliberately malformed ones.
 *
 * This is what to run on a physical machine BEFORE involving QEMU.  It answers
 * "is the broker's window up, is input reaching a client, and does a buffer
 * actually appear" in one screen, and it separates a broker problem from a
 * QEMU problem — which is otherwise two variables at once.
 *
 *     nvkvm-broker-testclient /run/nvkvm/display.sock
 *     nvkvm-broker-testclient /run/nvkvm/display.sock --present 640x480
 *
 * The --bad-* options exist for selftest.sh: each one is a specific way a
 * hostile VMM could lie to the privileged side, and each must be REJECTED.
 * They are in the test client rather than in a separate fuzzer because the
 * thing being tested is the broker's reaction to a well-formed connection that
 * then misbehaves, which is exactly what a compromised VMM is.
 *
 *   --bad-size H       claim height H for a buffer that is smaller (A-18)
 *   --bad-fourcc       claim a format the display never advertised
 *   --bad-dim          claim dimensions past NVKVM_BROKER_MAX_DIM
 *   --bad-fd           send a pipe instead of a dma-buf
 *   --bad-frame        send a truncated, wrongly sized message
 *   --bad-two-fds      attach two fds to one ATTACH
 *   --bad-reserved     set a reserved field
 *   --bad-commit-fd    attach an fd to a COMMIT
 *   --window WxH       send a WINDOW resize request
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include "../common/nvkvm_broker_proto.h"

#define FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
     ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define MOD_LINEAR   0ULL

static const char *evname(int t)
{
    switch (t) {
    case NVKVM_BROKER_EV_HELLO:   return "HELLO";
    case NVKVM_BROKER_EV_SURFACE: return "SURFACE";
    case NVKVM_BROKER_EV_FRAME:   return "FRAME";
    case NVKVM_BROKER_EV_RELEASE: return "RELEASE";
    case NVKVM_BROKER_EV_KEY:     return "KEY";
    case NVKVM_BROKER_EV_BTN:     return "BTN";
    case NVKVM_BROKER_EV_ABS:     return "ABS";
    case NVKVM_BROKER_EV_REL:     return "REL";
    case NVKVM_BROKER_EV_WHEEL:   return "WHEEL";
    case NVKVM_BROKER_EV_GRAB:    return "GRAB";
    case NVKVM_BROKER_EV_FOCUS:   return "FOCUS";
    case NVKVM_BROKER_EV_POINTER: return "POINTER";
    case NVKVM_BROKER_EV_BYE:     return "BYE";
    default:                      return "?";
    }
}

/* Send one command, optionally with one fd (or two, for --bad-two-fds). */
static int send_cmd(int sock, const struct nvkvm_broker_cmd *c,
                    const int *fds, int nfd, size_t bytes)
{
    char cbuf[CMSG_SPACE(sizeof(int) * 2)];
    struct iovec iov = { .iov_base = (void *)c, .iov_len = bytes };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
    ssize_t n;

    if (nfd > 0) {
        struct cmsghdr *cm;

        memset(cbuf, 0, sizeof(cbuf));
        msg.msg_control = cbuf;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)nfd);
        cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfd);
        memcpy(CMSG_DATA(cm), fds, sizeof(int) * (size_t)nfd);
    }
    n = sendmsg(sock, &msg, MSG_NOSIGNAL);
    if (n < 0) {
        perror("sendmsg");
        return -1;
    }
    return 0;
}

/*
 * A stand-in scanout buffer.  A memfd, not a real dma-buf — which is exactly
 * why only --backend test accepts it: on any real backend this is REJECTED,
 * and proving that is one of the selftest's checks.
 */
static int make_buffer(unsigned w, unsigned h, unsigned *stride_out)
{
    unsigned stride = w * 4;
    size_t size = (size_t)stride * h;
    int fd = memfd_create("nvkvm-broker-testbuf", MFD_CLOEXEC);

    if (fd < 0) {
        perror("memfd_create");
        return -1;
    }
    if (ftruncate(fd, (off_t)size) < 0) {
        perror("ftruncate");
        close(fd);
        return -1;
    }
    *stride_out = stride;
    return fd;
}

int main(int argc, char **argv)
{
    struct sockaddr_un sa;
    int sock, i;
    unsigned pw = 0, ph = 0, ww = 0, wh = 0;
    int bad_size = 0, bad_fourcc = 0, bad_dim = 0, bad_fd = 0, bad_frame = 0;
    int bad_two = 0, bad_reserved = 0, bad_commit_fd = 0;
    int quiet_after = 0;

    /* Line-buffered: this tool is normally watched live and normally ended
     * with Ctrl-C or timeout(1), and a fully buffered pipe would throw away
     * everything it had printed. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc < 2) {
        fprintf(stderr, "usage: %s <socket> [--present WxH] [--window WxH]\n"
                        "       [--bad-size H|--bad-fourcc|--bad-dim|"
                        "--bad-fd|--bad-frame|--bad-two-fds|--bad-reserved|"
                        "--bad-commit-fd]\n", argv[0]);
        return 2;
    }
    for (i = 2; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (!strcmp(a, "--present") && v)      { sscanf(v, "%ux%u", &pw, &ph); i++; }
        else if (!strcmp(a, "--window") && v)  { sscanf(v, "%ux%u", &ww, &wh); i++; }
        else if (!strcmp(a, "--bad-size") && v){ bad_size = atoi(v); i++; }
        else if (!strcmp(a, "--bad-fourcc"))   { bad_fourcc = 1; }
        else if (!strcmp(a, "--bad-dim"))      { bad_dim = 1; }
        else if (!strcmp(a, "--bad-fd"))       { bad_fd = 1; }
        else if (!strcmp(a, "--bad-frame"))    { bad_frame = 1; }
        else if (!strcmp(a, "--bad-two-fds"))  { bad_two = 1; }
        else if (!strcmp(a, "--bad-reserved")) { bad_reserved = 1; }
        else if (!strcmp(a, "--bad-commit-fd")){ bad_commit_fd = 1; }
        else { fprintf(stderr, "unknown option %s\n", a); return 2; }
    }
    if (!pw) { pw = 640; }
    if (!ph) { ph = 480; }

    sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, argv[1], sizeof(sa.sun_path) - 1);
    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("connect");
        return 1;
    }

    for (;;) {
        struct nvkvm_broker_pkt p;
        ssize_t n = recv(sock, &p, sizeof(p), 0);

        if (n == 0) {
            printf("broker closed the connection\n");
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("recv");
            break;
        }
        if (n != (ssize_t)NVKVM_BROKER_PKT_SIZE) {
            fprintf(stderr, "FRAMING ERROR: %zd bytes, expected %u — "
                    "disconnecting rather than resyncing\n",
                    n, NVKVM_BROKER_PKT_SIZE);
            break;
        }
        printf("seq=%-6u %-8s flags=%s%s x=%d y=%d w0=%u w1=%u\n",
               p.seq, evname(p.type),
               (p.flags & NVKVM_BROKER_F_GRABBED) ? "G" : "-",
               (p.flags & NVKVM_BROKER_F_FOCUSED) ? "F" : "-",
               p.x, p.y, p.w0, p.w1);

        if (p.type == NVKVM_BROKER_EV_HELLO) {
            printf("  proto v%u caps 0x%x: kbd=%d abs=%d rel=%d lock=%d "
                   "total-grab=%d focus=%d fs=%d dmabuf=%d modifiers=%d "
                   "release=%d\n", p.w0, p.w1,
                   !!(p.w1 & NVKVM_BROKER_CAP_KEYBOARD),
                   !!(p.w1 & NVKVM_BROKER_CAP_ABS_POINTER),
                   !!(p.w1 & NVKVM_BROKER_CAP_REL_POINTER),
                   !!(p.w1 & NVKVM_BROKER_CAP_POINTER_LOCK),
                   !!(p.w1 & NVKVM_BROKER_CAP_TOTAL_GRAB),
                   !!(p.w1 & NVKVM_BROKER_CAP_FOCUS_EVENTS),
                   !!(p.w1 & NVKVM_BROKER_CAP_FULLSCREEN),
                   !!(p.w1 & NVKVM_BROKER_CAP_DMABUF),
                   !!(p.w1 & NVKVM_BROKER_CAP_MODIFIERS),
                   !!(p.w1 & NVKVM_BROKER_CAP_RELEASE));
            if (p.w0 != NVKVM_BROKER_PROTO_VERSION) {
                fprintf(stderr, "protocol version mismatch: broker %u, this "
                        "client %u\n", p.w0, NVKVM_BROKER_PROTO_VERSION);
                break;
            }
        }

        /* Everything below happens ONCE, on the first FRAME: that is the
         * broker saying "you may draw", which is where a real client starts. */
        if (p.type != NVKVM_BROKER_EV_FRAME || quiet_after) {
            continue;
        }
        quiet_after = 1;

        if (ww && wh) {
            struct nvkvm_broker_cmd c = {
                .type = NVKVM_BROKER_CMD_WINDOW, .width = ww, .height = wh,
            };
            printf("  -> WINDOW %ux%u\n", ww, wh);
            send_cmd(sock, &c, NULL, 0, sizeof(c));
        }
        if (bad_frame) {
            struct nvkvm_broker_cmd c = { .type = NVKVM_BROKER_CMD_COMMIT };
            printf("  -> a %u-byte message (the protocol says %u)\n",
                   (unsigned)sizeof(c) - 3, NVKVM_BROKER_CMD_SIZE);
            /* Short, and then nothing more: the broker must not act on it and
             * must not be left half-parsed for the next client. */
            send_cmd(sock, &c, NULL, 0, sizeof(c) - 3);
            continue;
        }
        if (bad_commit_fd) {
            int fds[1];
            struct nvkvm_broker_cmd c = { .type = NVKVM_BROKER_CMD_COMMIT };
            unsigned st;

            fds[0] = make_buffer(16, 16, &st);
            printf("  -> COMMIT carrying an fd\n");
            send_cmd(sock, &c, fds, 1, sizeof(c));
            close(fds[0]);
            continue;
        }
        {
            struct nvkvm_broker_cmd c = { .type = NVKVM_BROKER_CMD_ATTACH };
            int fds[2] = { -1, -1 };
            int nfd = 1;
            unsigned stride = 0;

            if (bad_fd) {
                int pipefd[2];

                if (pipe(pipefd) < 0) {
                    perror("pipe");
                    break;
                }
                fds[0] = pipefd[0];
                close(pipefd[1]);
                stride = pw * 4;
                printf("  -> ATTACH with a PIPE instead of a dma-buf\n");
            } else {
                fds[0] = make_buffer(pw, ph, &stride);
                if (fds[0] < 0) {
                    break;
                }
            }
            c.width = pw;
            c.height = ph;
            c.stride = stride;
            c.offset = 0;
            c.fourcc = FOURCC('X', 'R', '2', '4');
            c.modifier = MOD_LINEAR;
            c.seq = 1;

            if (bad_size) {
                c.height = (uint32_t)bad_size;
                printf("  -> ATTACH claiming height %d for a %u-row buffer "
                       "(A-18)\n", bad_size, ph);
            }
            if (bad_fourcc) {
                c.fourcc = FOURCC('N', 'V', '1', '2');
                printf("  -> ATTACH claiming an unadvertised fourcc\n");
            }
            if (bad_dim) {
                c.width = NVKVM_BROKER_MAX_DIM + 1;
                printf("  -> ATTACH claiming width %u\n", c.width);
            }
            if (bad_reserved) {
                c.reserved1 = 1;
                printf("  -> ATTACH with a reserved field set\n");
            }
            if (bad_two) {
                unsigned st2;

                fds[1] = make_buffer(16, 16, &st2);
                nfd = 2;
                printf("  -> ATTACH carrying two fds\n");
            }
            if (!bad_size && !bad_fourcc && !bad_dim && !bad_fd &&
                !bad_reserved && !bad_two) {
                printf("  -> ATTACH %ux%u stride=%u XR24 linear\n",
                       pw, ph, stride);
            }
            send_cmd(sock, &c, fds, nfd, sizeof(c));
            for (i = 0; i < nfd; i++) {
                if (fds[i] >= 0) {
                    close(fds[i]);
                }
            }
        }
        {
            struct nvkvm_broker_cmd c = { .type = NVKVM_BROKER_CMD_COMMIT };

            printf("  -> COMMIT\n");
            send_cmd(sock, &c, NULL, 0, sizeof(c));
        }
    }
    return 0;
}
