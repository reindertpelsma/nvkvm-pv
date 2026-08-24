/*
 * prime_export_cost.c — what does nvkvm's per-frame PRESENT export actually cost?
 *
 * nvkvm_req_present() calls nvkvm_isolate_present_export() on every flip.  That
 * is a synchronous round-trip to the isolate stub which does
 * DRM_IOCTL_PRIME_HANDLE_TO_FD and returns the fd over SCM_RIGHTS, and it runs
 * on the virtio TX thread with the BQL held — so its cost is guest vCPU stall
 * time, once per frame.
 *
 * Measuring it in situ needs the whole stack (a desktop guest under nested KVM).
 * This measures the two components that dominate it, on any box with an NVIDIA
 * GPU and no virtualisation at all:
 *
 *   A  local   PRIME_HANDLE_TO_FD + close, in-process — the stub's actual work.
 *   B  ipc     the same thing across a process boundary: request over a unix
 *              socket, child exports, fd returned via SCM_RIGHTS, parent blocks
 *              on the reply.  Same shape as the real path.
 *
 * B minus A is the IPC overhead; B is what a frame pays.  Compare B against the
 * frame budget (16.7 ms at 60 Hz, 6.9 ms at 144 Hz) to decide whether caching
 * the fd is worth anything.
 *
 * What this does NOT measure: contention on the isolate's present_lock when
 * frames overlap, and real BQL interaction.  Both make the true cost HIGHER
 * than B, so B is a lower bound.
 *
 * Build:  cc -O2 -o prime_export_cost prime_export_cost.c $(pkg-config --cflags --libs libdrm gbm)
 * Run:    ./prime_export_cost [iterations]
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <gbm.h>

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static void report(const char *name, double *v, int n)
{
    double sum = 0;
    qsort(v, n, sizeof(v[0]), cmp_d);
    for (int i = 0; i < n; i++) {
        sum += v[i];
    }
    printf("%-6s n=%d  mean=%8.1f  p50=%8.1f  p90=%8.1f  p99=%8.1f  max=%8.1f us\n",
           name, n, sum / n, v[n / 2], v[(int)(n * 0.90)],
           v[(int)(n * 0.99)], v[n - 1]);
    printf("       %% of a 60Hz frame (16667us): mean %.2f%%  p99 %.2f%%\n",
           100.0 * (sum / n) / 16667.0, 100.0 * v[(int)(n * 0.99)] / 16667.0);
}

/* Find the first NVIDIA render node by asking sysfs which driver owns it. */
static int open_nvidia_render(char *path, size_t plen)
{
    for (int n = 128; n < 192; n++) {
        char sp[96], link[256], *base;
        ssize_t r;

        snprintf(sp, sizeof(sp), "/sys/class/drm/renderD%d/device/driver", n);
        r = readlink(sp, link, sizeof(link) - 1);
        if (r <= 0) {
            continue;
        }
        link[r] = '\0';
        base = strrchr(link, '/');
        base = base ? base + 1 : link;
        if (strcmp(base, "nvidia") != 0) {
            continue;
        }
        snprintf(path, plen, "/dev/dri/renderD%d", n);
        return open(path, O_RDWR | O_CLOEXEC);
    }
    return -1;
}

static int send_fd(int sock, int fd)
{
    char c = 'x';
    struct iovec iov = { .iov_base = &c, .iov_len = 1 };
    union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr a; } u;
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1,
                        .msg_control = u.buf, .msg_controllen = sizeof(u.buf) };
    struct cmsghdr *c2;

    memset(&u, 0, sizeof(u));
    c2 = CMSG_FIRSTHDR(&m);
    c2->cmsg_level = SOL_SOCKET;
    c2->cmsg_type = SCM_RIGHTS;
    c2->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c2), &fd, sizeof(int));
    return sendmsg(sock, &m, 0) < 0 ? -1 : 0;
}

static int recv_fd(int sock)
{
    char c;
    struct iovec iov = { .iov_base = &c, .iov_len = 1 };
    union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr a; } u;
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1,
                        .msg_control = u.buf, .msg_controllen = sizeof(u.buf) };
    struct cmsghdr *c2;
    int fd = -1;

    if (recvmsg(sock, &m, 0) <= 0) {
        return -1;
    }
    for (c2 = CMSG_FIRSTHDR(&m); c2; c2 = CMSG_NXTHDR(&m, c2)) {
        if (c2->cmsg_level == SOL_SOCKET && c2->cmsg_type == SCM_RIGHTS) {
            memcpy(&fd, CMSG_DATA(c2), sizeof(int));
        }
    }
    return fd;
}

int main(int argc, char **argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 2000;
    char path[64];
    int drm = open_nvidia_render(path, sizeof(path));
    struct gbm_device *gbm = NULL;
    struct gbm_bo *bo = NULL;
    uint32_t handle = 0;
    const char *how = "gbm scanout+rendering";

    if (drm < 0) {
        fprintf(stderr, "no NVIDIA render node (%s)\n", strerror(errno));
        return 1;
    }
    printf("node: %s\n", path);

    gbm = gbm_create_device(drm);
    if (gbm) {
        bo = gbm_bo_create(gbm, 1920, 1080, GBM_FORMAT_XRGB8888,
                           GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        if (!bo) {
            bo = gbm_bo_create(gbm, 1920, 1080, GBM_FORMAT_XRGB8888,
                               GBM_BO_USE_RENDERING);
            how = "gbm rendering";
        }
        if (bo) {
            handle = gbm_bo_get_handle(bo).u32;
        }
    }
    if (!handle) {
        /* Fallback: a dumb buffer still exercises the same export path. */
        struct drm_mode_create_dumb cd = { .width = 1920, .height = 1080, .bpp = 32 };
        if (drmIoctl(drm, DRM_IOCTL_MODE_CREATE_DUMB, &cd) == 0) {
            handle = cd.handle;
            how = "dumb buffer";
        }
    }
    if (!handle) {
        fprintf(stderr, "could not allocate a bo (gbm=%p): %s\n",
                (void *)gbm, strerror(errno));
        return 1;
    }
    printf("buffer: 1920x1080 XRGB8888 via %s, gem handle 0x%x\n", how, handle);
    printf("NOTE: this is a LOWER BOUND — it excludes present_lock contention\n"
           "      and BQL interaction, both of which only add.\n\n");

    double *a = calloc(iters, sizeof(double));
    double *b = calloc(iters, sizeof(double));

    /* ── A: local export ─────────────────────────────────────────────────── */
    for (int i = 0; i < iters; i++) {
        int fd = -1;
        double t0 = now_us();
        int r = drmPrimeHandleToFD(drm, handle, DRM_CLOEXEC, &fd);
        a[i] = now_us() - t0;
        if (r == 0 && fd >= 0) {
            close(fd);
        } else if (i == 0) {
            fprintf(stderr, "PRIME_HANDLE_TO_FD failed: %s\n", strerror(errno));
            return 1;
        }
    }

    /* ── B: across a process boundary, like QEMU <-> stub ────────────────── */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        return 1;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        for (;;) {
            char req;
            if (read(sv[1], &req, 1) != 1) {
                _exit(0);
            }
            int fd = -1;
            drmPrimeHandleToFD(drm, handle, DRM_CLOEXEC, &fd);
            send_fd(sv[1], fd);
            if (fd >= 0) {
                close(fd);
            }
        }
    }
    close(sv[1]);
    for (int i = 0; i < iters; i++) {
        char req = 'q';
        double t0 = now_us();
        if (write(sv[0], &req, 1) != 1) {
            break;
        }
        int fd = recv_fd(sv[0]);
        b[i] = now_us() - t0;
        if (fd >= 0) {
            close(fd);
        }
    }
    close(sv[0]);
    waitpid(pid, NULL, 0);

    report("local", a, iters);
    printf("\n");
    report("ipc", b, iters);
    printf("\nipc - local (the process boundary) ~= %.1f us at the median\n",
           b[iters / 2] - a[iters / 2]);
    return 0;
}
