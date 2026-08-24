/* SPDX-License-Identifier: GPL-2.0 OR Apache-2.0 */
/*
 * dmabuf_source.c — produce a REAL GPU dma-buf and either present it to the
 * broker or serve it over a socket.
 *
 * WHY THIS EXISTS.  testclient.c sends a memfd, which every real backend
 * correctly REJECTS ("the fd is not a dma-buf").  That proves the validator
 * and nothing else: it can never put a pixel on screen.  To get first light
 * without standing up the whole guest/isolate stack, something has to
 * allocate a genuine dma-buf on the real GPU, with a real modifier, and hand
 * it over.  That is all this does.
 *
 * It is TEST TOOLING.  It links libgbm; the broker does not, and must not.
 *
 * Two modes, and the split is the point:
 *
 *   --present SOCK   allocate, draw, ATTACH+COMMIT straight to the broker.
 *                    This process has the graphics stack.  Answers "does the
 *                    import path work on real hardware".
 *
 *   --serve SOCK     allocate, draw, then hand the fd (SCM_RIGHTS) plus its
 *                    descriptor to whoever connects.  This is a stand-in for
 *                    the ISOLATE: in the real system the buffer is allocated
 *                    behind the VMM and arrives as an fd over a socket, which
 *                    is exactly why the VMM needs no graphics stack.  Pair it
 *                    with `testclient --from SOCK`, which links nothing.
 *
 * Modifier selection is the interesting knob on NVIDIA:
 *   --modifier default   plain gbm_bo_create — whatever the driver picks
 *                        (block-linear on NVIDIA)
 *   --modifier linear    DRM_FORMAT_MOD_LINEAR, via create_with_modifiers
 *   --modifier implicit  plain gbm_bo_create, but ADVERTISED as
 *                        DRM_FORMAT_MOD_INVALID (DRI3 1.0 / implicit path)
 *   --modifier 0x...     one explicit modifier, via create_with_modifiers
 *   --as-implicit        allocate however --modifier says, but advertise
 *                        DRM_FORMAT_MOD_INVALID.  Separating the two matters
 *                        on a server that has no DRI3 1.2 at all: the only
 *                        question left is whether the implicit path can carry
 *                        a buffer whose real layout is NOT linear.
 *   --list               print what the render node actually hands back
 *
 * FILLING A TILED BUFFER, WITHOUT A GL CONTEXT.  gbm_bo_map refuses a
 * block-linear NVIDIA bo, so a CPU pattern cannot be written into the buffer
 * that actually matters.  A zeroed tiled buffer presents as a black window,
 * which is indistinguishable from "the import silently did nothing" — the
 * exact ambiguity this whole exercise exists to remove.  --fill-via-x11
 * resolves it by making the X SERVER do the writing: this tool imports the
 * same dma-buf as its own DRI3 pixmap and draws the pattern into it with core
 * X rendering, so the driver performs the tiling.  What then appears in the
 * broker's window is a pattern that only correct block-linear handling on BOTH
 * ends can produce.
 */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <gbm.h>

#ifdef SRC_HAVE_X11
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#endif

#include "../common/nvkvm_broker_proto.h"

#define MOD_INVALID  0x00ffffffffffffffULL
#define MOD_LINEAR   0ULL

#define FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | \
     ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* The descriptor handed to a --from peer along with the fd. */
struct src_desc {
    uint32_t width, height, stride, offset, fourcc;
    uint64_t modifier;
    uint64_t size;
};

static int send_fd(int sock, const void *buf, size_t len, int fd)
{
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { .iov_base = (void *)buf, .iov_len = len };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
    struct cmsghdr *cm;

    memset(cbuf, 0, sizeof(cbuf));
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);
    cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &fd, sizeof(int));
    return sendmsg(sock, &msg, 0) < 0 ? -1 : 0;
}

static int connect_unix(const char *path)
{
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    int s = socket(AF_UNIX, SOCK_STREAM, 0);

    if (s < 0) {
        return -1;
    }
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

/* A recognisable pattern: colour ramp, a red border and moving diagonal
 * stripes, so a wrong stride, a wrong offset, or a tiled buffer being read as
 * linear is obvious at a glance rather than "looks a bit odd". */
static void draw(void *base, uint32_t w, uint32_t h, uint32_t stride,
                 unsigned frame)
{
    uint32_t xx, yy;

    for (yy = 0; yy < h; yy++) {
        uint32_t *row = (uint32_t *)((char *)base + (size_t)yy * stride);

        for (xx = 0; xx < w; xx++) {
            uint32_t r = (xx * 255) / (w ? w : 1);
            uint32_t g = (yy * 255) / (h ? h : 1);
            uint32_t b = 0x40;

            if (((xx + yy + frame * 8) % 128) < 8) {
                r = 0xff; g = 0xff; b = 0xff;         /* diagonal stripes    */
            }
            if (yy < 24 || yy >= h - 24 || xx < 24 || xx >= w - 24) {
                r = 0xff; g = 0x00; b = 0x00;         /* red border          */
            }
            row[xx] = 0xff000000u | (r << 16) | (g << 8) | b;
        }
    }
}

/*
 * Draw the same pattern into the dma-buf by importing it as an X pixmap and
 * using core X rendering, so the X server (and therefore the NVIDIA driver)
 * does the tiling.  Returns true if the fill was actually performed.
 */
static bool fill_via_x11(int dmafd, uint32_t w, uint32_t h, uint32_t stride,
                         uint32_t offset, uint64_t modifier)
{
#ifndef SRC_HAVE_X11
    (void)dmafd; (void)w; (void)h; (void)stride; (void)offset; (void)modifier;
    printf("  --fill-via-x11: not compiled in (needs xcb + xcb-dri3)\n");
    return false;
#else
    xcb_connection_t *c = xcb_connect(NULL, NULL);
    xcb_screen_t *scr;
    xcb_pixmap_t pix;
    xcb_gcontext_t gc;
    xcb_void_cookie_t ck;
    xcb_generic_error_t *err;
    int dup2fd, i;

    if (!c || xcb_connection_has_error(c)) {
        printf("  --fill-via-x11: cannot connect to X\n");
        return false;
    }
    scr = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    dup2fd = fcntl(dmafd, F_DUPFD_CLOEXEC, 0);
    if (dup2fd < 0) {
        return false;
    }
    pix = xcb_generate_id(c);
    if (modifier == MOD_INVALID) {
        ck = xcb_dri3_pixmap_from_buffer_checked(
                 c, pix, scr->root, (uint32_t)(stride * h + offset),
                 (uint16_t)w, (uint16_t)h, (uint16_t)stride, 24, 32, dup2fd);
    } else {
        int32_t fds[1] = { dup2fd };

        ck = xcb_dri3_pixmap_from_buffers_checked(
                 c, pix, scr->root, 1, (uint16_t)w, (uint16_t)h,
                 stride, offset, 0, 0, 0, 0, 0, 0, 24, 32, modifier, fds);
    }
    err = xcb_request_check(c, ck);
    if (err) {
        printf("  --fill-via-x11: the X server REFUSED the same buffer "
               "(error %u, major %u minor %u)\n", err->error_code,
               err->major_code, err->minor_code);
        free(err);
        xcb_disconnect(c);
        return false;
    }

    gc = xcb_generate_id(c);
    xcb_create_gc(c, gc, pix, 0, NULL);

    /* Colour bars, then a red border, then diagonal stripes: the same shape
     * as the CPU pattern, drawn by the server so the tiling is the driver's
     * problem rather than ours. */
    for (i = 0; i < 8; i++) {
        static const uint32_t bar[8] = {
            0x00202020, 0x00c00000, 0x0000c000, 0x000000c0,
            0x00c0c000, 0x0000c0c0, 0x00c000c0, 0x00e0e0e0,
        };
        uint32_t v = bar[i];
        xcb_rectangle_t r = { (int16_t)(i * (int)w / 8), 0,
                              (uint16_t)(w / 8 + 1), (uint16_t)h };

        xcb_change_gc(c, gc, XCB_GC_FOREGROUND, &v);
        xcb_poly_fill_rectangle(c, pix, gc, 1, &r);
    }
    {
        uint32_t red = 0x00ff0000;
        xcb_rectangle_t b[4] = {
            { 0, 0, (uint16_t)w, 24 },
            { 0, (int16_t)(h - 24), (uint16_t)w, 24 },
            { 0, 0, 24, (uint16_t)h },
            { (int16_t)(w - 24), 0, 24, (uint16_t)h },
        };

        xcb_change_gc(c, gc, XCB_GC_FOREGROUND, &red);
        xcb_poly_fill_rectangle(c, pix, gc, 4, b);
    }
    {
        uint32_t white = 0x00ffffff;
        int k;

        xcb_change_gc(c, gc, XCB_GC_FOREGROUND, &white);
        for (k = -(int)h; k < (int)w; k += 128) {
            xcb_point_t seg[2] = { { (int16_t)k, 0 },
                                   { (int16_t)(k + (int)h), (int16_t)h } };

            xcb_poly_line(c, XCB_COORD_MODE_ORIGIN, pix, gc, 2, seg);
        }
    }
    xcb_free_gc(c, gc);
    xcb_free_pixmap(c, pix);
    xcb_flush(c);
    /* A round trip, so the drawing has certainly been executed before the
     * buffer is handed to the broker. */
    free(xcb_get_input_focus_reply(c, xcb_get_input_focus(c), NULL));
    xcb_disconnect(c);
    printf("  --fill-via-x11: the X server imported the SAME dma-buf and drew "
           "into it\n");
    return true;
#endif
}

static void list_modifiers(struct gbm_device *dev)
{
    struct gbm_bo *bo;
    uint64_t lin = MOD_LINEAR;
    struct gbm_bo *b2;

    /* gbm has no enumeration API, so the honest way to learn what the driver
     * will give you is to ask for a buffer and read back what it chose. */
    bo = gbm_bo_create(dev, 256, 256, GBM_FORMAT_XRGB8888,
                       GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!bo) {
        printf("gbm_bo_create(SCANOUT|RENDERING) failed: %s\n",
               strerror(errno));
        bo = gbm_bo_create(dev, 256, 256, GBM_FORMAT_XRGB8888,
                           GBM_BO_USE_RENDERING);
    }
    if (!bo) {
        printf("gbm_bo_create(RENDERING) failed too: %s\n", strerror(errno));
    } else {
        printf("default gbm_bo_create modifier: 0x%016" PRIx64
               "  planes=%d stride=%u\n",
               gbm_bo_get_modifier(bo), gbm_bo_get_plane_count(bo),
               gbm_bo_get_stride(bo));
        gbm_bo_destroy(bo);
    }

    b2 = gbm_bo_create_with_modifiers(dev, 256, 256, GBM_FORMAT_XRGB8888,
                                      &lin, 1);
    printf("DRM_FORMAT_MOD_LINEAR accepted by gbm: %s\n", b2 ? "yes" : "NO");
    if (b2) {
        printf("  -> read back 0x%016" PRIx64 " stride=%u\n",
               gbm_bo_get_modifier(b2), gbm_bo_get_stride(b2));
        gbm_bo_destroy(b2);
    }
}

int main(int argc, char **argv)
{
    const char *node = "/dev/dri/renderD128";
    const char *present_sock = NULL, *serve_sock = NULL;
    const char *modstr = "default";
    uint32_t w = 1280, h = 720;
    unsigned frames = 1;
    bool do_list = false, advertise_implicit = false, fill_x11 = false;
    int drmfd, i;
    struct gbm_device *dev;
    struct gbm_bo *bo = NULL;
    struct src_desc d;
    int dmafd;
    void *map = NULL, *mapdata = NULL;
    uint32_t maps = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (!strcmp(a, "--node") && v)          { node = v; i++; }
        else if (!strcmp(a, "--present") && v)  { present_sock = v; i++; }
        else if (!strcmp(a, "--serve") && v)    { serve_sock = v; i++; }
        else if (!strcmp(a, "--modifier") && v) { modstr = v; i++; }
        else if (!strcmp(a, "--size") && v)     { sscanf(v, "%ux%u", &w, &h); i++; }
        else if (!strcmp(a, "--frames") && v)   { frames = (unsigned)atoi(v); i++; }
        else if (!strcmp(a, "--as-implicit"))   { advertise_implicit = true; }
        else if (!strcmp(a, "--fill-via-x11"))  { fill_x11 = true; }
        else if (!strcmp(a, "--list"))          { do_list = true; }
        else {
            fprintf(stderr,
                    "usage: %s [--node /dev/dri/renderDN] "
                    "[--present SOCK | --serve SOCK | --list]\n"
                    "          [--modifier default|linear|implicit|0xHEX] "
                    "[--size WxH] [--frames N]\n", argv[0]);
            return 2;
        }
    }

    drmfd = open(node, O_RDWR | O_CLOEXEC);
    if (drmfd < 0) {
        fprintf(stderr, "open %s: %s\n", node, strerror(errno));
        return 1;
    }
    dev = gbm_create_device(drmfd);
    if (!dev) {
        fprintf(stderr, "gbm_create_device failed on %s\n", node);
        return 1;
    }
    printf("gbm backend: %s\n", gbm_device_get_backend_name(dev));

    if (do_list) {
        list_modifiers(dev);
        return 0;
    }
    if (!present_sock && !serve_sock) {
        fprintf(stderr, "one of --present, --serve or --list is required\n");
        return 2;
    }

    if (!strcmp(modstr, "default") || !strcmp(modstr, "implicit")) {
        advertise_implicit |= !strcmp(modstr, "implicit");
        bo = gbm_bo_create(dev, w, h, GBM_FORMAT_XRGB8888,
                           GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        if (!bo) {
            fprintf(stderr, "gbm_bo_create with SCANOUT failed (%s); "
                    "retrying RENDERING only\n", strerror(errno));
            bo = gbm_bo_create(dev, w, h, GBM_FORMAT_XRGB8888,
                               GBM_BO_USE_RENDERING);
        }
    } else {
        uint64_t mod = !strcmp(modstr, "linear")
                     ? MOD_LINEAR : strtoull(modstr, NULL, 0);

        bo = gbm_bo_create_with_modifiers(dev, w, h, GBM_FORMAT_XRGB8888,
                                          &mod, 1);
        if (!bo) {
            fprintf(stderr, "gbm_bo_create_with_modifiers(0x%016" PRIx64
                    ") REFUSED by the driver\n", mod);
            return 1;
        }
    }
    if (!bo) {
        fprintf(stderr, "gbm_bo_create failed: %s\n", strerror(errno));
        return 1;
    }

    memset(&d, 0, sizeof(d));
    d.width    = w;
    d.height   = h;
    d.stride   = gbm_bo_get_stride(bo);
    d.offset   = gbm_bo_get_offset(bo, 0);
    d.fourcc   = FOURCC('X', 'R', '2', '4');
    d.modifier = advertise_implicit ? MOD_INVALID : gbm_bo_get_modifier(bo);

    dmafd = gbm_bo_get_fd(bo);
    if (dmafd < 0) {
        fprintf(stderr, "gbm_bo_get_fd failed: %s\n", strerror(errno));
        return 1;
    }
    d.size = (uint64_t)lseek(dmafd, 0, SEEK_END);

    printf("allocated %ux%u XR24  stride=%u offset=%u planes=%d\n",
           w, h, d.stride, d.offset, gbm_bo_get_plane_count(bo));
    printf("  real modifier   0x%016" PRIx64 "\n", gbm_bo_get_modifier(bo));
    printf("  advertised as   0x%016" PRIx64 "%s\n", d.modifier,
           advertise_implicit ? "  (DRM_FORMAT_MOD_INVALID, implicit path)"
                              : "");
    printf("  dma-buf size    %" PRIu64 " bytes (need >= %" PRIu64 ")\n",
           d.size, (uint64_t)d.stride * h + d.offset);

    if (fill_x11) {
        fill_via_x11(dmafd, w, h, d.stride, d.offset, gbm_bo_get_modifier(bo));
        goto filled;
    }

    /* Try to put a recognisable image in it.  On a tiled buffer the driver may
     * refuse the map, or may detile for us; either way, say which happened. */
    map = gbm_bo_map(bo, 0, 0, w, h, GBM_BO_TRANSFER_WRITE, &maps, &mapdata);
    if (map) {
        printf("  gbm_bo_map OK (map stride %u) — drawing a test pattern\n",
               maps);
        draw(map, w, h, maps, 0);
    } else {
        printf("  gbm_bo_map REFUSED (%s) — the buffer holds whatever the\n"
               "  allocator left in it.  The IMPORT is the thing under test;\n"
               "  noise on screen still means the frame arrived.\n",
               strerror(errno));
    }

filled:
    if (serve_sock) {
        struct sockaddr_un sa = { .sun_family = AF_UNIX };
        int ls, cs;
        mode_t old = umask(0077);

        unlink(serve_sock);
        ls = socket(AF_UNIX, SOCK_STREAM, 0);
        snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", serve_sock);
        if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
            listen(ls, 4) < 0) {
            fprintf(stderr, "bind/listen %s: %s\n", serve_sock,
                    strerror(errno));
            return 1;
        }
        umask(old);
        chmod(serve_sock, 0666);   /* test tooling: let a container uid in */
        printf("serving the dma-buf on %s — this process holds the graphics\n"
               "stack so that the peer does not have to.\n", serve_sock);
        fflush(stdout);
        for (;;) {
            cs = accept(ls, NULL, NULL);
            if (cs < 0) {
                if (errno == EINTR) { continue; }
                break;
            }
            if (send_fd(cs, &d, sizeof(d), dmafd) < 0) {
                fprintf(stderr, "send_fd: %s\n", strerror(errno));
            } else {
                printf("handed the dma-buf fd to a peer\n");
                fflush(stdout);
            }
            close(cs);
        }
        return 0;
    }

    /* --present: talk to the broker directly. */
    {
        int s = connect_unix(present_sock);
        struct nvkvm_broker_pkt pkt;
        struct nvkvm_broker_cmd c;
        unsigned f;

        if (s < 0) {
            fprintf(stderr, "connect %s: %s\n", present_sock, strerror(errno));
            return 1;
        }
        if (read(s, &pkt, sizeof(pkt)) != (ssize_t)sizeof(pkt) ||
            pkt.type != NVKVM_BROKER_EV_HELLO) {
            fprintf(stderr, "no HELLO from the broker\n");
            return 1;
        }
        printf("HELLO: proto %u caps 0x%04x  (DMABUF=%d MODIFIERS=%d)\n",
               pkt.w0, pkt.w1, !!(pkt.w1 & NVKVM_BROKER_CAP_DMABUF),
               !!(pkt.w1 & NVKVM_BROKER_CAP_MODIFIERS));

        for (f = 0; f < frames; f++) {
            if (map) {
                draw(map, w, h, maps, f);
            }
            memset(&c, 0, sizeof(c));
            c.type = NVKVM_BROKER_CMD_ATTACH;
            c.width = d.width; c.height = d.height;
            c.stride = d.stride; c.offset = d.offset;
            c.fourcc = d.fourcc; c.modifier = d.modifier;
            c.seq = f;
            if (send_fd(s, &c, sizeof(c), dmafd) < 0) {
                fprintf(stderr, "ATTACH send: %s\n", strerror(errno));
                return 1;
            }
            memset(&c, 0, sizeof(c));
            c.type = NVKVM_BROKER_CMD_COMMIT;
            c.seq = f;
            if (write(s, &c, sizeof(c)) != (ssize_t)sizeof(c)) {
                fprintf(stderr, "COMMIT send: %s\n", strerror(errno));
                return 1;
            }
            printf("sent ATTACH+COMMIT #%u\n", f);
            fflush(stdout);
            usleep(200000);
        }
        printf("frames sent.  Reading events — a REJECTION is reported on the\n"
               "BROKER's stderr, not here.\n");
        for (;;) {
            ssize_t n = read(s, &pkt, sizeof(pkt));

            if (n <= 0) {
                printf("broker closed the connection\n");
                break;
            }
            if (pkt.type == NVKVM_BROKER_EV_SURFACE) {
                printf("SURFACE %dx%d\n", pkt.x, pkt.y);
            } else if (pkt.type == NVKVM_BROKER_EV_RELEASE) {
                printf("RELEASE buffer %llu\n",
                       (unsigned long long)pkt.w0 |
                       ((unsigned long long)pkt.w1 << 32));
            } else if (pkt.type == NVKVM_BROKER_EV_BYE) {
                printf("BYE reason %d\n", pkt.x);
                break;
            }
        }
    }
    return 0;
}
