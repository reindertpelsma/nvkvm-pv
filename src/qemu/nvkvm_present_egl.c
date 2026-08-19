/*
 * nvkvm_present_egl.c — host-side capture of a guest-composited scanout buffer
 * (#107 present path C).
 *
 * The guest's virtual KMS head flips an NVIDIA scanout bo; the stub PRIME-exports
 * it as a host dma-buf and QEMU receives the fd (#106).  Here QEMU imports that
 * dma-buf as a GL texture on the host GPU (NVIDIA EGL detiles block-linear
 * automatically) and reads it back to CPU memory so it can be written to a file
 * or, later, handed to a QemuConsole (dpy_gl_scanout_dmabuf) / NVENC.
 *
 * QEMU is the trusted VMM, so it may open the host render node for its own
 * display compositing — this is distinct from the per-guest sandboxed stub.
 *
 * Reuses QEMU's ui/egl-helpers.c (egl_init / egl_dmabuf_import_texture /
 * egl_fb_read), the same path virtio-gpu uses, so headless GBM + DRM modifiers
 * are handled for us.
 */
#include "qemu/osdep.h"
#include "virtio_nvgpu.h"   /* NVKVM_QEMU_GRAPHICS compile-time gate */
#include "nvkvm_drm_node.h"

#if defined(CONFIG_OPENGL) && NVKVM_QEMU_GRAPHICS
#include "ui/console.h"
#include "ui/surface.h"
#include "ui/egl-helpers.h"
#include "ui/dmabuf.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "hw/qdev-core.h"
#include "nvkvm_log.h"

#include "nvkvm_present_egl.h"

/* One EGL display/context for the whole VMM, lazily created on the first
 * present, current on the thread that first calls in (QEMU's virtio TX thread —
 * present is dispatched inline there).  -1 = not tried, 0 = failed (give up),
 * 1 = ready. */
static int nvkvm_egl_state = -1;

static bool nvkvm_present_egl_ensure(void)
{
    if (nvkvm_egl_state == 1) {
        return true;
    }
    if (nvkvm_egl_state == 0) {
        return false;   /* already failed once — don't spam retries */
    }

    Error *err = NULL;
    char nodebuf[64];
    const char *node = nvkvm_nvidia_render_path(0, nodebuf, sizeof(nodebuf));

    if (!node) {
        fprintf(stderr, "nvkvm present: no NVIDIA DRM render node on this host\n");
        nvkvm_egl_state = 0;
        return false;
    }
    /* Headless: GBM rendernode platform, surfaceless context (no X needed). */
    if (!egl_init(node, DISPLAY_GL_MODE_ON, &err)) {
        fprintf(stderr, "nvkvm present: egl_init failed: %s\n",
                err ? error_get_pretty(err) : "(unknown)");
        error_free(err);
        nvkvm_egl_state = 0;
        return false;
    }
    if (!qemu_egl_has_dmabuf()) {
        fprintf(stderr, "nvkvm present: host EGL lacks dma_buf import\n");
        nvkvm_egl_state = 0;
        return false;
    }
    nvkvm_egl_state = 1;
    fprintf(stderr, "nvkvm present: host EGL ready (dma-buf import)\n");
    return true;
}

/* Diagnostic dma-buf import: same single-plane attr set as QEMU's
 * egl_dmabuf_import_texture, but logs eglGetError() and the exact attributes on
 * failure so we can chase the NVIDIA block-linear import requirement (#107).
 * Returns a GL texture name, or 0 on failure (after logging why). */
static uint32_t nvkvm_import_dmabuf_tex(int fd, uint32_t width, uint32_t height,
                                        uint32_t stride, uint32_t fourcc,
                                        uint64_t modifier)
{
    EGLint attrs[64];
    int i = 0;

    attrs[i++] = EGL_WIDTH;                      attrs[i++] = width;
    attrs[i++] = EGL_HEIGHT;                     attrs[i++] = height;
    attrs[i++] = EGL_LINUX_DRM_FOURCC_EXT;       attrs[i++] = fourcc;
    attrs[i++] = EGL_DMA_BUF_PLANE0_FD_EXT;      attrs[i++] = fd;
    attrs[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;   attrs[i++] = stride;
    attrs[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;  attrs[i++] = 0;
#ifdef EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
    if (modifier) {
        attrs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attrs[i++] = (EGLint)(modifier & 0xffffffff);
        attrs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attrs[i++] = (EGLint)((modifier >> 32) & 0xffffffff);
    }
#endif
    attrs[i++] = EGL_NONE;

    fprintf(stderr,
            "nvkvm present: import fd=%d %ux%u stride=%u fourcc=0x%08x "
            "modifier=0x%016llx\n",
            fd, width, height, stride, fourcc,
            (unsigned long long)modifier);

    EGLImageKHR image = eglCreateImageKHR(qemu_egl_display, EGL_NO_CONTEXT,
                                          EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    if (image == EGL_NO_IMAGE_KHR) {
        fprintf(stderr,
                "nvkvm present: eglCreateImageKHR FAILED, eglGetError=0x%04x\n",
                (unsigned)eglGetError());
        return 0;
    }

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)image);
    GLenum glerr = glGetError();
    eglDestroyImageKHR(qemu_egl_display, image);
    if (glerr != GL_NO_ERROR) {
        fprintf(stderr,
                "nvkvm present: glEGLImageTargetTexture2DOES glGetError=0x%04x\n",
                (unsigned)glerr);
        glDeleteTextures(1, &texture);
        return 0;
    }
    fprintf(stderr, "nvkvm present: import OK tex=%u\n", texture);
    return texture;
}

/* Write a DisplaySurface (pixman ARGB32 / BGRA bytes) to a binary PPM (P6). */
static int nvkvm_write_ppm(const char *path, DisplaySurface *s)
{
    int w = surface_width(s), h = surface_height(s);
    int stride = surface_stride(s);
    const uint8_t *data = surface_data(s);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -errno;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        const uint8_t *row = data + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            /* egl_fb_read gives GL_BGRA bytes → B,G,R,A in memory. */
            uint8_t b = row[x * 4 + 0];
            uint8_t g = row[x * 4 + 1];
            uint8_t r = row[x * 4 + 2];
            fputc(r, f); fputc(g, f); fputc(b, f);
        }
    }
    fclose(f);
    return 0;
}

int nvkvm_present_capture(int dmabuf_fd, uint32_t width, uint32_t height,
                          uint32_t stride, uint32_t fourcc, uint64_t modifier,
                          const char *out_path)
{
    if (!nvkvm_present_egl_ensure()) {
        return -ENOTSUP;
    }
    if (width == 0 || height == 0) {
        return -EINVAL;
    }

    /* The present is dispatched on a QEMU isolate/virtio worker thread, which
     * is generally NOT the thread egl_init made the context current on.  EGL
     * contexts are per-thread, so we must bind qemu_egl_rn_ctx on THIS thread
     * (else glGenTextures silently returns 0).  Released at exit so the next
     * present's (possibly different) thread can claim it.  Presents are
     * throttled/serialized, so no two threads contend here in practice. */
    if (!eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        qemu_egl_rn_ctx)) {
        fprintf(stderr,
                "nvkvm present: eglMakeCurrent failed, eglGetError=0x%04x\n",
                (unsigned)eglGetError());
        return -EIO;
    }

    /* qemu_dmabuf_new dups nothing — it takes ownership of fd? No: it stores fd
     * and qemu_dmabuf_free()/close() manage it.  We pass a dup so our caller's
     * fd lifetime is independent. */
    int dup_fd = dup(dmabuf_fd);
    if (dup_fd < 0) {
        int e = -errno;
        eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        return e;
    }
    QemuDmaBuf *buf = qemu_dmabuf_new(width, height, stride, 0, 0,
                                      width, height, fourcc, modifier,
                                      dup_fd, false, false);
    if (!buf) {
        close(dup_fd);
        eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        return -ENOMEM;
    }

    int ret = 0;
    uint32_t tex = nvkvm_import_dmabuf_tex(dup_fd, width, height, stride,
                                           fourcc, modifier);
    if (!tex) {
        ret = -EIO;
        goto out;
    }

    egl_fb fb = { 0 };
    egl_fb_setup_for_tex(&fb, width, height, tex, false);

    DisplaySurface *s = qemu_create_displaysurface(width, height);
    if (!s) {
        ret = -ENOMEM;
        goto out_fb;
    }
    egl_fb_read(s, &fb);          /* glReadPixels texture → CPU (BGRA) */
    ret = nvkvm_write_ppm(out_path, s);
    qemu_free_displaysurface(s);

out_fb:
    egl_fb_destroy(&fb);
    glDeleteTextures(1, &tex);
out:
    qemu_dmabuf_close(buf);
    qemu_dmabuf_free(buf);
    /* Release the context from this thread so the next present can bind it. */
    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    return ret;
}

/* ───────────────────────── live present-to-window (#102) ──────────────────
 *
 * The guest's virtual KMS head flips a scanout bo every frame; nvkvm_req_present
 * exports it as a host dma-buf and calls nvkvm_present_submit() from a virtio
 * worker thread.  We stash that frame (owning its fd) in a per-device slot and
 * poke the display.  The QemuConsole's gfx_update callback — run on the main
 * loop under the BQL, where it is safe to touch the dpy_ calls and the UI GL
 * context — then drains the slot and pushes the frame to the active display.
 *
 * Decoupling the worker-thread submit from the main-thread present via the slot
 * is what makes this correct: display ops never run off-main-thread, and the
 * host refresh cadence naturally paces consumption (the seed of #108).
 */

#include <pthread.h>
#include <string.h>

typedef struct NvkvmPresent {
    QemuConsole *con;
    DeviceState *dev;

    pthread_mutex_t lock;
    /* Pending frame from the guest; owns `fd`.  fd<0 ⇒ slot empty. */
    int      fd;
    uint32_t w, h, stride, fourcc;
    uint64_t modifier;
    bool     dirty;          /* a new frame is waiting since last present */

    QEMUBH  *bh;             /* scheduled from worker → graphic_hw_update */

    /* GL zero-copy path: the dma-buf currently handed to the UI.  Kept alive
     * (and its fd open) until a newer frame replaces it, like virtio-gpu's
     * primary dmabuf. */
    QemuDmaBuf *cur_buf;

    /* Readback path: CPU surface we glReadPixels into and hand to the 2D
     * display.  Recreated only when the geometry changes. */
    DisplaySurface *surface;
    uint32_t surf_w, surf_h;

    int  mode;               /* -1 undecided, 0 readback, 1 GL zero-copy */
} NvkvmPresent;

/* Decide GL-zero-copy vs readback once, then cache.  Zero-copy is only valid
 * when the active UI console has GL *and* its renderer is the same NVIDIA GPU
 * the scanout buffer came from — a cross-vendor (Intel/AMD/llvmpipe) UI cannot
 * import an NVIDIA dma-buf, so those fall back to readback (always correct).
 * NVKVM_PRESENT_MODE=gl|readback overrides the probe. */
static int nvkvm_present_decide_mode(NvkvmPresent *p)
{
    if (p->mode != -1) {
        return p->mode;
    }

    const char *forced = getenv("NVKVM_PRESENT_MODE");
    bool has_gl = console_has_gl(p->con);

    if (forced && !strcmp(forced, "readback")) {
        p->mode = 0;
    } else if (forced && !strcmp(forced, "gl")) {
        /* Operator asserts the window renders on the same NVIDIA GPU. */
        p->mode = has_gl ? 1 : 0;
    } else {
        /*
         * Auto: default to readback — the universal, always-correct floor.
         * Zero-copy GL scanout only works when the window's *own* GL renderer
         * can import an NVIDIA dma-buf, i.e. the window is rendered on the same
         * NVIDIA GPU.  We cannot reliably read the window's renderer vendor
         * through QEMU's console API (the render-node EGL ctx we can query is
         * NOT the window's context — e.g. on a software/Xvfb or Intel/AMD host
         * the window is llvmpipe/Mesa while the render node is still NVIDIA, and
         * a wrong "GL" choice yields a blank window).  So we do NOT guess GL
         * from the render node; readback always shows the frame.  An operator on
         * a confirmed NVIDIA-rendered host desktop sets NVKVM_PRESENT_MODE=gl for
         * the zero-copy fast path.  (A future refinement can probe the window's
         * context directly and auto-upgrade.)
         */
        p->mode = 0;
    }

    fprintf(stderr, "nvkvm present: window mode=%s (console_has_gl=%d)\n",
            p->mode ? "GL-zerocopy" : "readback", has_gl);
    return p->mode;
}

/* Push the pending frame to the GL console with no CPU copy. */
static void nvkvm_present_gl(NvkvmPresent *p, int fd, uint32_t w, uint32_t h,
                             uint32_t stride, uint32_t fourcc, uint64_t mod)
{
    /* qemu_dmabuf_new takes ownership of fd (freed by qemu_dmabuf_close). */
    QemuDmaBuf *buf = qemu_dmabuf_new(w, h, stride, 0, 0, w, h, fourcc, mod,
                                      fd, false, false);
    if (!buf) {
        close(fd);
        return;
    }
    qemu_console_resize(p->con, w, h);
    dpy_gl_scanout_dmabuf(p->con, buf);
    dpy_gl_update(p->con, 0, 0, w, h);

    /* Retire the previously scanned-out buffer only now that the new one is
     * live (its fd was in use until this point). */
    if (p->cur_buf) {
        qemu_dmabuf_close(p->cur_buf);
        qemu_dmabuf_free(p->cur_buf);
    }
    p->cur_buf = buf;
}

/* Import the pending frame on our private NVIDIA EGL context, read it back to a
 * CPU surface, and push it to the 2D display.  Works for any host display GPU. */
static void nvkvm_present_readback(NvkvmPresent *p, int fd, uint32_t w,
                                   uint32_t h, uint32_t stride, uint32_t fourcc,
                                   uint64_t mod)
{
    if (!nvkvm_present_egl_ensure()) {
        close(fd);
        return;
    }
    if (!eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        qemu_egl_rn_ctx)) {
        close(fd);
        return;
    }

    uint32_t tex = nvkvm_import_dmabuf_tex(fd, w, h, stride, fourcc, mod);
    if (!tex) {
        goto out_ctx;
    }

    egl_fb fb = { 0 };
    egl_fb_setup_for_tex(&fb, w, h, tex, false);

    if (!p->surface || p->surf_w != w || p->surf_h != h) {
        if (p->surface) {
            qemu_free_displaysurface(p->surface);
        }
        p->surface = qemu_create_displaysurface(w, h);
        p->surf_w = w;
        p->surf_h = h;
        qemu_console_resize(p->con, w, h);
        dpy_gfx_replace_surface(p->con, p->surface);
    }
    egl_fb_read(p->surface, &fb);          /* glReadPixels texture → CPU BGRA */
    egl_fb_destroy(&fb);
    glDeleteTextures(1, &tex);
    dpy_gfx_update(p->con, 0, 0, w, h);

out_ctx:
    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    close(fd);                             /* readback owns + consumes the fd */
}

/* GraphicHwOps.gfx_update — main loop, BQL held.  Drain the slot and present. */
static void nvkvm_present_gfx_update(void *opaque)
{
    NvkvmPresent *p = opaque;

    pthread_mutex_lock(&p->lock);
    if (!p->dirty || p->fd < 0) {
        pthread_mutex_unlock(&p->lock);
        return;
    }
    int      fd     = p->fd;
    uint32_t w      = p->w,  h = p->h, stride = p->stride, fourcc = p->fourcc;
    uint64_t mod    = p->modifier;
    p->fd    = -1;            /* take ownership of this frame's fd */
    p->dirty = false;
    pthread_mutex_unlock(&p->lock);

    if (nvkvm_present_decide_mode(p) == 1) {
        nvkvm_present_gl(p, fd, w, h, stride, fourcc, mod);
    } else {
        nvkvm_present_readback(p, fd, w, h, stride, fourcc, mod);
    }
}

static void nvkvm_present_invalidate(void *opaque)
{
    (void)opaque;            /* full redraw happens on the next flip */
}

static const GraphicHwOps nvkvm_present_hwops = {
    .invalidate  = nvkvm_present_invalidate,
    .gfx_update  = nvkvm_present_gfx_update,
};

/* Bottom half: runs on the main loop, pokes the display to drain the slot
 * promptly (lower latency than waiting for the periodic refresh tick). */
static void nvkvm_present_bh(void *opaque)
{
    NvkvmPresent *p = opaque;
    graphic_hw_update(p->con);
}

int nvkvm_present_console_init(struct DeviceState *dev, struct VirtIONvgpu *nv)
{
    NvkvmPresent *p = g_new0(NvkvmPresent, 1);
    pthread_mutex_init(&p->lock, NULL);
    p->fd   = -1;
    p->mode = -1;
    p->dev  = dev;
    p->con  = graphic_console_init(dev, 0, &nvkvm_present_hwops, p);
    p->bh   = qemu_bh_new(nvkvm_present_bh, p);
    nv->present_ctx = p;
    fprintf(stderr, "nvkvm present: registered QemuConsole for guest GPU "
            "scanout (window display)\n");
    return 0;
}

void nvkvm_present_console_fini(struct VirtIONvgpu *nv)
{
    NvkvmPresent *p = nv->present_ctx;
    if (!p) {
        return;
    }
    nv->present_ctx = NULL;
    if (p->bh) {
        qemu_bh_delete(p->bh);
    }
    if (p->con) {
        graphic_console_close(p->con);
    }
    if (p->cur_buf) {
        qemu_dmabuf_close(p->cur_buf);
        qemu_dmabuf_free(p->cur_buf);
    }
    if (p->surface) {
        qemu_free_displaysurface(p->surface);
    }
    if (p->fd >= 0) {
        close(p->fd);
    }
    pthread_mutex_destroy(&p->lock);
    g_free(p);
}

bool nvkvm_present_submit(struct VirtIONvgpu *nv, int dmabuf_fd,
                          uint32_t width, uint32_t height, uint32_t stride,
                          uint32_t fourcc, uint64_t modifier)
{
    NvkvmPresent *p = nv->present_ctx;
    if (!p || !p->con) {
        return false;        /* no console → caller keeps the fd */
    }

    pthread_mutex_lock(&p->lock);
    if (p->fd >= 0) {
        close(p->fd);        /* drop the frame the display hasn't taken yet */
    }
    p->fd       = dmabuf_fd;
    p->w        = width;
    p->h        = height;
    p->stride   = stride;
    p->fourcc   = fourcc;
    p->modifier = modifier;
    p->dirty    = true;
    pthread_mutex_unlock(&p->lock);

    qemu_bh_schedule(p->bh);
    return true;
}

#else /* !CONFIG_OPENGL || !NVKVM_QEMU_GRAPHICS */
#include "nvkvm_present_egl.h"
int nvkvm_present_capture(int dmabuf_fd, uint32_t width, uint32_t height,
                          uint32_t stride, uint32_t fourcc, uint64_t modifier,
                          const char *out_path)
{
    (void)dmabuf_fd; (void)width; (void)height; (void)stride;
    (void)fourcc; (void)modifier; (void)out_path;
    return -ENOTSUP;
}
int nvkvm_present_console_init(struct DeviceState *dev, struct VirtIONvgpu *nv)
{
    (void)dev; (void)nv;
    return -ENOTSUP;
}
void nvkvm_present_console_fini(struct VirtIONvgpu *nv) { (void)nv; }
bool nvkvm_present_submit(struct VirtIONvgpu *nv, int dmabuf_fd,
                          uint32_t width, uint32_t height, uint32_t stride,
                          uint32_t fourcc, uint64_t modifier)
{
    (void)nv; (void)dmabuf_fd; (void)width; (void)height;
    (void)stride; (void)fourcc; (void)modifier;
    return false;            /* not accepted → caller closes the fd */
}
#endif
