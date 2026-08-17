/*
 * wcapflip.c — capture a headless weston's composited output into an nvkvm GPU
 * dma-buf (proxy GEM), then flip it on the virtual KMS head so the present path
 * (#106) delivers the live desktop/game frame to the host.  (#110)
 *
 * WHY: a DRM-backend compositor on NVIDIA hangs in libnvidia-egl-gbm's
 * scanout-present path (coupled to nvidia-modeset, which we don't forward).  A
 * HEADLESS GL compositor renders the desktop offscreen with no KMS scanout and
 * works.  This client bridges the two worlds: it uses weston_output_capture_v1
 * to have weston (which has a valid GL context) blit its composited framebuffer
 * into a buffer WE allocate on card0, then drives our virtual KMS head with that
 * buffer.  weston composites; we present.  No NVKMS, no EGL re-import in-client.
 *
 * Flow per frame:
 *   weston_capture_source_v1.capture(buf) -> 'complete'
 *     -> drmModeSetCrtc/PageFlip(buf's fb) on the virtual head
 *        -> nvkvm_pipe_update -> nvkvm_virtio_present -> host
 *
 * The capture buffer is a gbm bo (GBM_BO_USE_SCANOUT|LINEAR) on card0: a proxy
 * GEM carrying a stub_handle, so it is both compositor-writable (dmabuf) and
 * flippable (AddFB2).  weston's required capture format modifier is LINEAR,
 * which is also what our present path / host import want.
 *
 *   usage: wcapflip [card=/dev/dri/card0] [nframes=120] [--shm]
 *     --shm : capture into a wl_shm buffer instead of dmabuf (fallback path;
 *             proves capture works even if NVIDIA rejects dmabuf capture).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/udmabuf.h>
#include <time.h>
#include <wayland-client.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include "weston-output-capture-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

/* ── Wayland globals ─────────────────────────────────────────────────────── */
static struct wl_output             *output;
static struct weston_capture_v1     *capture_factory;
static struct zwp_linux_dmabuf_v1   *dmabuf_factory;
static struct wl_shm                *shm;

/* capture source state */
static uint32_t cap_format;     /* DRM fourcc weston wants            */
static int      cap_w, cap_h;   /* size weston wants                  */
static int      cap_ready;      /* got initial format+size            */
static int      cap_done;       /* 'complete' for the in-flight shot  */
static int      cap_retry;      /* 'retry'                            */
static int      cap_failed;     /* 'failed'                           */

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t ver)
{
    (void)d; (void)ver;
    if (!strcmp(iface, wl_output_interface.name))
        output = wl_registry_bind(r, name, &wl_output_interface, 1);
    else if (!strcmp(iface, weston_capture_v1_interface.name))
        capture_factory = wl_registry_bind(r, name, &weston_capture_v1_interface, 1);
    else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name))
        dmabuf_factory = wl_registry_bind(r, name, &zwp_linux_dmabuf_v1_interface, 3);
    else if (!strcmp(iface, wl_shm_interface.name))
        shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{ (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

/* ── capture source events ───────────────────────────────────────────────── */
static void cap_fmt(void *d, struct weston_capture_source_v1 *s, uint32_t fmt)
{ (void)d; (void)s; cap_format = fmt; }
static void cap_size(void *d, struct weston_capture_source_v1 *s, int32_t w, int32_t h)
{ (void)d; (void)s; cap_w = w; cap_h = h; cap_ready = 1; }
static void cap_complete(void *d, struct weston_capture_source_v1 *s)
{ (void)d; (void)s; cap_done = 1; }
static void cap_retry_(void *d, struct weston_capture_source_v1 *s)
{ (void)d; (void)s; cap_retry = 1; }
static void cap_failed_(void *d, struct weston_capture_source_v1 *s, const char *msg)
{ (void)d; (void)s; cap_failed = 1; fprintf(stderr, "capture failed: %s\n", msg ? msg : "(null)"); }
static const struct weston_capture_source_v1_listener cap_listener = {
    cap_fmt, cap_size, cap_complete, cap_retry_, cap_failed_,
};

/* a buffer (dmabuf-backed gbm bo + its KMS fb, or shm) */
struct capbuf {
    struct wl_buffer *wlbuf;
    /* dmabuf path */
    struct gbm_bo *bo;
    uint32_t fb_id, handle, stride;
    /* shm / udmabuf path */
    void *shm_data; size_t shm_size; int memfd;
};

static int g_card_fd;
static struct gbm_device *g_gbm;

static int make_dmabuf(struct capbuf *cb)
{
    /* Allocate XRGB8888 + SCANOUT|RENDERING — the NVIDIA-gbm recipe that yields
     * a linear, scanout-capable bo (proven by gbmflip).  weston requires the
     * capture buffer in its own format (ARGB8888) with a LINEAR modifier; XRGB
     * and ARGB share byte layout, so we allocate XRGB (scanout-friendly) and
     * label the dmabuf wl_buffer as weston's cap_format. */
    /* weston capture requires a LINEAR-modifier buffer; NVIDIA gbm defaults a
     * SCANOUT bo to block-linear, so request LINEAR explicitly. */
    const uint64_t lin = DRM_FORMAT_MOD_LINEAR;
    cb->bo = gbm_bo_create_with_modifiers(g_gbm, cap_w, cap_h, DRM_FORMAT_XRGB8888,
                                          &lin, 1);
    if (!cb->bo)
        cb->bo = gbm_bo_create(g_gbm, cap_w, cap_h, DRM_FORMAT_XRGB8888,
                               GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!cb->bo) { fprintf(stderr, "gbm_bo_create(LINEAR) failed\n"); return -1; }
    cb->stride = gbm_bo_get_stride(cb->bo);
    cb->handle = gbm_bo_get_handle(cb->bo).u32;
    uint64_t mod = gbm_bo_get_modifier(cb->bo);
    fprintf(stderr, "scanout bo: %dx%d stride=%u modifier=0x%llx\n",
            cap_w, cap_h, cb->stride, (unsigned long long)mod);
    int fd = gbm_bo_get_fd(cb->bo);
    if (fd < 0) { fprintf(stderr, "gbm_bo_get_fd failed\n"); return -1; }

    struct zwp_linux_buffer_params_v1 *p = zwp_linux_dmabuf_v1_create_params(dmabuf_factory);
    zwp_linux_buffer_params_v1_add(p, fd, 0, 0, cb->stride,
                                   (uint32_t)(mod >> 32), (uint32_t)(mod & 0xffffffff));
    cb->wlbuf = zwp_linux_buffer_params_v1_create_immed(p, cap_w, cap_h, cap_format, 0);
    zwp_linux_buffer_params_v1_destroy(p);
    close(fd);
    if (!cb->wlbuf) { fprintf(stderr, "dmabuf create_immed failed\n"); return -1; }

    /* KMS framebuffer for flipping (XRGB — alpha ignored on scanout) */
    uint32_t hs[4] = { cb->handle, 0, 0, 0 }, ss[4] = { cb->stride, 0, 0, 0 }, os[4] = {0};
    if (drmModeAddFB2(g_card_fd, cap_w, cap_h, DRM_FORMAT_XRGB8888, hs, ss, os, &cb->fb_id, 0)) {
        fprintf(stderr, "AddFB2 failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* udmabuf: a dma-buf backed by a memfd (real guest pages).  weston's NVIDIA EGL
 * imports it as an EXTERNAL dma-buf → pins the memfd pages → the forwarded RM
 * OS_DESCRIPTOR path migrates those same pages into the stub (the mechanism that
 * already backs userptr/OS_DESCRIPTOR ioctls).  Tests whether a memfd-backed
 * buffer is importable where a hollow proxy-GEM dma-buf is not. */
static int make_udmabuf(struct capbuf *cb)
{
    int stride = cap_w * 4;
    size_t sz = (size_t)stride * cap_h;
    sz = (sz + 0xfff) & ~0xfffUL;                  /* page-align (udmabuf req) */
    cb->shm_size = sz;
    cb->memfd = memfd_create("wcap-udmabuf", MFD_ALLOW_SEALING | MFD_CLOEXEC);
    if (cb->memfd < 0) { perror("memfd"); return -1; }
    if (ftruncate(cb->memfd, sz) < 0) { perror("ftruncate"); return -1; }
    if (fcntl(cb->memfd, F_ADD_SEALS, F_SEAL_SHRINK) < 0) { perror("F_SEAL_SHRINK"); return -1; }
    cb->shm_data = mmap(NULL, sz, PROT_READ|PROT_WRITE, MAP_SHARED, cb->memfd, 0);

    int udev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (udev < 0) { perror("open /dev/udmabuf"); return -1; }
    struct udmabuf_create uc = { .memfd = (uint32_t)cb->memfd,
                                 .flags = UDMABUF_FLAGS_CLOEXEC,
                                 .offset = 0, .size = sz };
    int dbuf = ioctl(udev, UDMABUF_CREATE, &uc);
    close(udev);
    if (dbuf < 0) { perror("UDMABUF_CREATE"); return -1; }
    fprintf(stderr, "udmabuf: %dx%d stride=%d size=%zu dmabuf_fd=%d\n",
            cap_w, cap_h, stride, sz, dbuf);

    struct zwp_linux_buffer_params_v1 *p = zwp_linux_dmabuf_v1_create_params(dmabuf_factory);
    zwp_linux_buffer_params_v1_add(p, dbuf, 0, 0, stride,
                                   DRM_FORMAT_MOD_LINEAR >> 32,
                                   DRM_FORMAT_MOD_LINEAR & 0xffffffff);
    cb->wlbuf = zwp_linux_buffer_params_v1_create_immed(p, cap_w, cap_h, cap_format, 0);
    zwp_linux_buffer_params_v1_destroy(p);
    close(dbuf);
    return cb->wlbuf ? 0 : -1;
}

static int make_shm(struct capbuf *cb)
{
    int stride = cap_w * 4;
    cb->shm_size = (size_t)stride * cap_h;
    char name[] = "/wcapflip-XXXXXX";
    int fd = memfd_create("wcapflip", 0);
    if (fd < 0) { perror("memfd"); return -1; }
    if (ftruncate(fd, cb->shm_size) < 0) { perror("ftruncate"); close(fd); return -1; }
    cb->shm_data = mmap(NULL, cb->shm_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    /* weston's capture format is ARGB8888 (DRM 0x34325241). WL_SHM ARGB8888==0. */
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, cb->shm_size);
    cb->wlbuf = wl_shm_pool_create_buffer(pool, 0, cap_w, cap_h, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    (void)name;
    return cb->wlbuf ? 0 : -1;
}

int main(int argc, char **argv)
{
    const char *card = "/dev/dri/card0";
    int nframes = 120, use_shm = 0, use_udmabuf = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shm")) use_shm = 1;
        else if (!strcmp(argv[i], "--udmabuf")) use_udmabuf = 1;
        else if (argv[i][0] == '/') card = argv[i];
        else nframes = atoi(argv[i]);
    }
    int cpu_read = use_shm || use_udmabuf;   /* CPU-mapped target, no KMS flip */

    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { fprintf(stderr, "wl_display_connect failed (WAYLAND_DISPLAY?)\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(dpy);
    if (!output || !capture_factory) {
        fprintf(stderr, "missing globals: output=%p capture=%p dmabuf=%p shm=%p\n",
                (void*)output, (void*)capture_factory, (void*)dmabuf_factory, (void*)shm);
        return 2;
    }
    if (!use_shm && !dmabuf_factory) { fprintf(stderr, "no linux-dmabuf; use --shm\n"); return 2; }
    if (use_shm && !shm) { fprintf(stderr, "no wl_shm\n"); return 2; }

    /* DRM master + gbm on card0 (virtual head is free — headless weston uses renderD128) */
    g_card_fd = open(card, O_RDWR | O_CLOEXEC);
    if (g_card_fd < 0) { perror("open card"); return 3; }
    drmSetMaster(g_card_fd);
    g_gbm = gbm_create_device(g_card_fd);
    if (!g_gbm) { fprintf(stderr, "gbm_create_device failed\n"); return 3; }

    /* find connector+crtc+mode of the virtual head */
    drmModeRes *res = drmModeGetResources(g_card_fd);
    drmModeConnector *conn = NULL;
    for (int i = 0; res && i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(g_card_fd, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes) { conn = c; break; }
        if (c) drmModeFreeConnector(c);
    }
    if (!conn) { fprintf(stderr, "no connected head\n"); return 3; }
    drmModeModeInfo mode = conn->modes[0];
    drmModeEncoder *enc = drmModeGetEncoder(g_card_fd, conn->encoders[0]);
    uint32_t crtc = (enc && enc->crtc_id) ? enc->crtc_id : res->crtcs[0];

    /* capture source: framebuffer = final composited fb, always available */
    struct weston_capture_source_v1 *src =
        weston_capture_v1_create(capture_factory, output,
                                 WESTON_CAPTURE_V1_SOURCE_FRAMEBUFFER);
    weston_capture_source_v1_add_listener(src, &cap_listener, NULL);
    wl_display_roundtrip(dpy);     /* get initial format + size */
    if (!cap_ready) { fprintf(stderr, "no format/size from capture source\n"); return 4; }
    const char *modename = use_udmabuf ? "udmabuf" : use_shm ? "shm" : "dmabuf";
    printf("capture: %dx%d fourcc=0x%08x (%s)\n", cap_w, cap_h, cap_format, modename);
    if (!cpu_read) cap_format = cap_format ? cap_format : DRM_FORMAT_XRGB8888;

    struct capbuf cb = { .memfd = -1 };
    int mret = use_udmabuf ? make_udmabuf(&cb) : use_shm ? make_shm(&cb) : make_dmabuf(&cb);
    if (mret) return 5;

    struct pollfd pfd = { .fd = g_card_fd, .events = POLLIN };
    int presented = 0, first = 1;
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int f = 0; f < nframes; f++) {
        cap_done = cap_retry = cap_failed = 0;
        weston_capture_source_v1_capture(src, cb.wlbuf);
        wl_display_flush(dpy);
        /* pump until the shot resolves */
        while (!cap_done && !cap_retry && !cap_failed) {
            if (wl_display_dispatch(dpy) < 0) { fprintf(stderr, "dispatch err\n"); goto out; }
        }
        if (cap_failed) { fprintf(stderr, "frame %d: capture failed\n", f); break; }
        if (cap_retry) {
            fprintf(stderr, "frame %d: retry (format/size changed to %dx%d 0x%x)\n",
                    f, cap_w, cap_h, cap_format);
            /* would realloc here; for the proof, bail */
            break;
        }
        /* composited frame now in cb. CPU-mapped (shm/udmabuf): dump first frame
         * as proof-of-content (P6 PPM, ARGB B,G,R,A in memory). dmabuf: flip. */
        if (cpu_read) {
            if (f == 0) {
                FILE *pp = fopen("/tmp/wcapflip_frame.ppm", "wb");
                if (pp) {
                    const uint8_t *px = cb.shm_data;
                    fprintf(pp, "P6\n%d %d\n255\n", cap_w, cap_h);
                    for (int i = 0; i < cap_w * cap_h; i++) {
                        fputc(px[i*4+2], pp); fputc(px[i*4+1], pp); fputc(px[i*4+0], pp);
                    }
                    fclose(pp);
                    fprintf(stderr, "wrote /tmp/wcapflip_frame.ppm (proof of content)\n");
                }
            }
        } else {
            int r;
            if (first) {
                r = drmModeSetCrtc(g_card_fd, crtc, cb.fb_id, 0, 0,
                                   &conn->connector_id, 1, &mode);
                first = 0;
            } else {
                r = drmModePageFlip(g_card_fd, crtc, cb.fb_id,
                                    DRM_MODE_PAGE_FLIP_EVENT, NULL);
                if (!r && poll(&pfd, 1, 100) > 0) {
                    drmEventContext ev = { .version = 2 };
                    drmHandleEvent(g_card_fd, &ev);
                }
            }
            if (r) { fprintf(stderr, "frame %d: flip/setcrtc: %s\n", f, strerror(errno)); break; }
        }
        presented++;
        if ((f % 30) == 0) printf("  presented %d frames\n", presented);
    }
out:;
    struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("RESULT presented=%d/%d mode=%s  %.1f fps (%.0f ms)\n",
           presented, nframes, modename,
           secs > 0 ? presented / secs : 0.0, secs * 1000);
    return presented > 0 ? 0 : 6;
}
