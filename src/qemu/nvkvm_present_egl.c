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
#include <time.h>
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
#include "nvkvm_udmabuf.h"

/* DRM_FORMAT_XRGB8888, spelled locally so this file needs no drm_fourcc.h. */
#define DRM_FORMAT_XR24_LOCAL 0x34325258u

/*
 * How many staging buffers the readback rotates through.
 *
 * The console path needs two: the main loop consumes a frame before the thread
 * can come back around to the one being shown.  THE BROKER PATH DOES NOT --
 * a compositor HOLDS a dma-buf across frames and releases it on its own
 * schedule, so writing into it while it is still being read is a real
 * corruption, not a theoretical one.  OBSERVED on the physical box with two
 * buffers: the broker's own glitch detector fired on EVERY frame
 *   "REUSE-IN-FLIGHT seq=3 buf=233 slot=2 (the compositor never released it)"
 * alternating between the two slots forever.
 *
 * Four gives the compositor room to hold one while another is in flight and a
 * third is being written, at 8 MB each for a 1080p head.
 */
#define NVKVM_STAGE_MAX 4u
#define NVKVM_STAGE_CONSOLE 2u
#include "nvkvm_display_relay.h"

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
                                        uint64_t modifier,
                                        EGLImageKHR *image_out)
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

    /*
     * Bind with glEGLImageTargetTexStorageEXT, NOT glEGLImageTargetTexture2DOES.
     *
     * NVIDIA rejects the legacy OES entry point for these dma-buf images:
     * measured on RTX 4070 / 595.84 it returns GL_INVALID_OPERATION (0x0502)
     * for GL_TEXTURE_2D, and the image binds only as GL_TEXTURE_EXTERNAL_OES
     * (renderbuffer import fails too).  That is what made the window black --
     * every frame was imported, rejected, and dropped.
     *
     * GL_EXT_EGL_image_storage is the modern path and takes the same image as
     * immutable GL_TEXTURE_2D storage, so no samplerExternalOES shader and no
     * ES context are needed.  Keep the OES call as a fallback for drivers that
     * offer only the old extension.
     */
    static PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC tex_storage;
    static bool tex_storage_looked_up;
    if (!tex_storage_looked_up) {
        tex_storage_looked_up = true;
        tex_storage = (PFNGLEGLIMAGETARGETTEXSTORAGEEXTPROC)
            eglGetProcAddress("glEGLImageTargetTexStorageEXT");
    }

    GLuint texture = 0;
    while (glGetError() != GL_NO_ERROR) { }
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    if (tex_storage) {
        tex_storage(GL_TEXTURE_2D, (GLeglImageOES)image, NULL);
    } else {
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)image);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GLenum glerr = glGetError();
    if (glerr != GL_NO_ERROR) {
        eglDestroyImageKHR(qemu_egl_display, image);
        fprintf(stderr,
                "nvkvm present: EGLImage->GL_TEXTURE_2D failed glGetError=0x%04x "
                "(tex_storage=%d)\n", (unsigned)glerr, tex_storage ? 1 : 0);
        glDeleteTextures(1, &texture);
        return 0;
    }
    /*
     * Hand the EGLImage back ALIVE.  With EXT_EGL_image_storage the image IS
     * the texture's immutable storage, so destroying it here leaves the
     * texture pointing at released driver state -- glDeleteTextures() then
     * faults inside libnvidia-eglcore (confirmed from a core dump: crash at
     * glDeleteTextures, top frames inside libnvidia-eglcore).  The caller
     * destroys it after the texture.
     */
    *image_out = image;
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
     * present's (possibly different) thread can claim it.
     *
     * STALE INVARIANT, 2026-08-23.  This used to end "presents are
     * throttled/serialized, so no two threads contend here in practice".  That
     * stopped being true when nvkvm_present_thread_fn() was added: it binds the
     * SAME context and holds it for the life of the VM.  An EGL context can be
     * current on only one thread at a time, so whichever of the two binds
     * second now fails -- observed as a black window on an SDL host.  Do not
     * rely on the old assumption; the two paths need to agree on an owner. */
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
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    uint32_t tex = nvkvm_import_dmabuf_tex(dup_fd, width, height, stride,
                                           fourcc, modifier, &image);
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
    eglDestroyImageKHR(qemu_egl_display, image);   /* after the texture */
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
#include "qemu/thread.h"

#define NVKVM_PRESENT_CACHE 8

typedef struct NvkvmPresent {
    QemuConsole *con;
    DeviceState *dev;
    struct VirtIONvgpu *nv;  /* for ui_info, which travels back to the guest */

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

    uint32_t key;            /* guest scanout bo this frame came from */
    uint32_t owner;          /* isolate that exported it (S-4) */

    /*
     * S-4: isolates whose cached imports are due to be dropped.  Isolate death
     * is reported from a virtio worker thread, but EGLImages and textures may
     * only be touched on the main loop with our context current, so the worker
     * records the id here and nvkvm_present_gfx_update() does the work.
     * `dead_all` is the overflow answer: forget everything rather than keep a
     * dead isolate's import alive because the list was full.
     */
    uint32_t dead[16];
    unsigned ndead;
    bool     dead_all;

    /*
     * Imported-buffer cache.  A compositor cycles a handful of scanout bos
     * (measured: 3), so import each ONE and reuse it, exactly as a host
     * compositor does.
     *
     * This is not only a speed matter.  Creating and destroying an EGLImage
     * plus its texture every frame at ~600 fps reliably killed the NVIDIA
     * driver: SIGSEGV inside libnvidia-eglcore, reached from
     * eglDestroyImageKHR/glDeleteTextures, within ~2 s of a compositor
     * starting.  Removing just the per-frame destroys made the same run
     * survive indefinitely (5107 frames, no fault), which is what identified
     * the churn rather than the import as the problem.
     */
    struct {
        bool        valid;
        /*
         * S-4: `key` is a GEM handle out of the OWNING stub's own drm_file
         * IDR, so it is unique per isolate and nothing more -- it starts at 1
         * in every one of them.  With the virtual head fixed at a single mode
         * and two modifiers, two compositors collide on key 1 with identical
         * geometry as a matter of course, not as a rare coincidence, and the
         * newcomer's frames then resolve to its predecessor's EGLImage.  The
         * owning isolate is the missing half of the identity.
         */
        uint32_t    owner;
        uint32_t    key, w, h, stride, fourcc;
        /*
         * The GEM handle is not an identity.  It comes out of an IDR, and an
         * IDR hands back the LOWEST FREE id -- so a compositor that frees its
         * scanout bo and allocates another gets the same `key` again, at the
         * same geometry, because same-size buffers are exactly what a
         * compositor cycles.  Matching on (owner, key, geometry) alone then
         * returns the PREVIOUS buffer's EGLImage and the window shows a stale
         * frame until some other handle happens to come along.
         *
         * `ino` is the identity the handle is not: every dma_buf gets its own
         * inode in the dma-buf pseudo-filesystem, stable across dup(2) and
         * SCM_RIGHTS, and a newly allocated buffer never reuses a live one's.
         * Cheap enough to check per frame (one fstat).
         */
        uint64_t    ino;
        uint64_t    mod;
        EGLImageKHR image;
        GLuint      tex;
        uint64_t    used;    /* p->tick when this entry was last returned */
    } cache[NVKVM_PRESENT_CACHE];
    uint64_t tick;           /* monotonic, bumped on every cache lookup */
    egl_fb fb;               /* reused; egl_fb_setup_for_tex keeps the FBO */

    /* Readback path: two pixel buffer objects, written one frame ahead of
     * being mapped.  See nvkvm_fb_read_async(). */
    GLuint   pbo[2];
    uint32_t pbo_w, pbo_h;   /* geometry the PBOs were sized for */
    unsigned pbo_idx;        /* the one this frame writes */
    bool     pbo_primed;     /* a transfer is in flight in the other one */

    /* #125: the readback path no longer touches a DisplaySurface at all -- it
     * fills `stage` below and the main loop wraps that.  The console still
     * owns the surface it is shown, and still frees the one it displaces. */

    int  mode;               /* -1 undecided, 0 readback, 1 GL zero-copy */
    char ui_renderer[128];   /* host UI's GL renderer, probed once (#125) */
    unsigned probe_tries;    /* #125: the UI may not be up on frame 1 */

    /*
     * #125: the readback path runs on its own thread.
     *
     * It used to run on the main loop out of gfx_update, which forced
     * eglMakeCurrent(our ctx) / eglMakeCurrent(NO_CONTEXT) around EVERY frame:
     * EGL current-ness is per-thread, the UI backend keeps its own context on
     * that same thread, and leaving ours bound breaks the UI's next draw.  So
     * the import, the readback and the 8 MB copy all happened with the BQL
     * held -- the guest's vCPUs stopped for the whole transfer -- and the two
     * contexts took turns on one thread, which is also what made a Mesa/Xvfb
     * host UI fail with EGL_BAD_ACCESS.
     *
     * Giving the NVIDIA context a thread of its own removes the conflict at
     * the root rather than working around it: our context is made current once
     * at thread start and stays current for the life of the VM, the UI's
     * context is never disturbed because it lives on a different thread, and
     * no display op ever runs off the main loop.  The thread hands finished
     * pixels back through `stage`, and the main loop only wraps them in a
     * DisplaySurface -- no pixel copy under the BQL at all.
     */
    QemuThread     thread;
    QemuSemaphore  wake;
    bool           thread_started;
    bool           thread_failed;
    int            thread_stop;

    /*
     * Double-buffered CPU staging.  The thread fills one buffer while the main
     * loop copies out of the other, so neither waits on the other and a frame
     * is never torn by a write landing mid-copy.
     */
    /*
     * CROSS-VENDOR PRESENT (readback for the broker).
     *
     * When the broker answers CMD_QUERY_FORMAT with "cannot show that", the
     * frame has to be detiled by the guest's own GPU into something any
     * compositor can import.  The staging pair is then backed by udmabuf, so
     * the SAME pages are a pointer we glReadPixels into and a LINEAR dma-buf
     * the broker hands to the compositor.  One GPU transfer, no CPU copy, no
     * compositor upload -- see nvkvm_udmabuf.h for the measurements.
     */
    bool      relay_readback;      /* stage into udmabuf and submit to relay */
    struct nvkvm_udmabuf stage_buf[NVKVM_STAGE_MAX];

    uint8_t  *stage[NVKVM_STAGE_MAX];
    unsigned  stage_n;       /* buffers actually in rotation */
    uint32_t  stage_w, stage_h;
    unsigned  stage_write;   /* buffer the thread writes next */
    unsigned  stage_ready;   /* buffer holding the newest finished frame */
    bool      stage_has_frame;
} NvkvmPresent;

/* DRM_FORMAT_MOD_LINEAR; a linear buffer is the one layout a non-NVIDIA host
 * GL stack can import, which is what makes cross-vendor zero-copy possible. */
#define NVKVM_MOD_LINEAR 0ULL

/* How many frames to keep re-probing the UI before settling on readback. */
#define NVKVM_PROBE_TRIES 120

/*
 * #125: ask the host UI what GPU actually renders its window.
 *
 * This is the question the mode decision always needed and could not answer.
 * The render-node EGL context we own is NVIDIA by construction, so querying it
 * tells us nothing about the window -- on an Intel/AMD/Xvfb host the window is
 * Mesa while the render node is still NVIDIA.  Guessing "GL" there produced a
 * blank window, which is why the old code simply defaulted to readback and
 * made the operator opt in to zero-copy by hand.
 *
 * dpy_gl_ctx_create() gives us a context on the UI's own display, so
 * glGetString() through it reports the window's renderer.  Safe to bind and
 * drop: both ui/gtk-egl.c and ui/sdl2-gl.c re-bind their context at the top of
 * every draw, so leaving no context current cannot strand them.  Runs once,
 * on the main loop, where the UI lives.
 */
static bool nvkvm_probe_ui_renderer(NvkvmPresent *p)
{
    QEMUGLParams params = { .major_ver = 2, .minor_ver = 0 };
    QEMUGLContext ctx;

    p->ui_renderer[0] = '\0';
    if (!console_has_gl(p->con)) {
        return false;
    }
    ctx = dpy_gl_ctx_create(p->con, &params);
    if (!ctx) {
        return false;
    }
    if (dpy_gl_ctx_make_current(p->con, ctx) != 0) {
        dpy_gl_ctx_destroy(p->con, ctx);
        return false;
    }
    const char *vendor   = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    snprintf(p->ui_renderer, sizeof(p->ui_renderer), "%s / %s",
             vendor ? vendor : "?", renderer ? renderer : "?");
    dpy_gl_ctx_destroy(p->con, ctx);
    return true;
}

/*
 * Decide GL-zero-copy vs readback once, then cache.  #125: this now probes the
 * host UI's renderer instead of refusing to guess.
 *
 * Zero-copy is valid when the UI's own GL stack can import the guest's scanout
 * dma-buf.  Two ways that holds:
 *   - the window renders on NVIDIA too, so it takes the buffer in its native
 *     block-linear layout; or
 *   - the buffer is LINEAR, which any vendor's GL can import.
 *
 * Be clear about the second one: it does NOT fire for a scanout buffer today,
 * and not by accident.  NVIDIA cannot use a LINEAR dma-buf as an EGLImage
 * render target (measured in-guest: GL_INVALID_OPERATION on bind, incomplete
 * FBO), which is how a compositor obtains its output framebuffer -- so the
 * guest deliberately advertises only block-linear scanout modifiers
 * (nvkvm_kms.c) and a guest compositor can never hand us a linear frame.  The
 * branch is kept because it is the correct answer for any linear buffer that
 * does reach here (the cursor plane accepts LINEAR), not because it makes an
 * Intel iGPU host zero-copy.
 *
 * So on a cross-vendor host the honest answer is readback, and that is what
 * gets chosen and reported.  Getting zero-copy there would need a detile blit
 * on our NVIDIA context into a linear buffer the other vendor can import,
 * which is a GPU pass plus an allocation both devices can reach -- a separate
 * piece of work, not a mode choice.
 *
 * NVKVM_PRESENT_MODE=gl|readback still overrides the probe.
 */
static int nvkvm_present_decide_mode(NvkvmPresent *p, uint64_t mod)
{
    if (p->mode != -1) {
        return p->mode;
    }

    const char *forced = getenv("NVKVM_PRESENT_MODE");
    bool has_gl  = console_has_gl(p->con);
    bool probed  = nvkvm_probe_ui_renderer(p);
    bool nvidia  = probed && (strcasestr(p->ui_renderer, "NVIDIA") != NULL);
    bool linear  = (mod == NVKVM_MOD_LINEAR);
    const char *why;

    if (forced && !strcmp(forced, "readback")) {
        p->mode = 0;  why = "forced by NVKVM_PRESENT_MODE=readback";
    } else if (forced && !strcmp(forced, "gl")) {
        p->mode = has_gl ? 1 : 0;
        why = has_gl ? "forced by NVKVM_PRESENT_MODE=gl"
                     : "NVKVM_PRESENT_MODE=gl ignored: UI console has no GL";
    } else if (!has_gl) {
        p->mode = 0;  why = "UI console has no GL (e.g. -display vnc/curses)";
    } else if (!probed) {
        /*
         * The probe runs on the first frame, and the UI backend may not have
         * its window (and therefore its GL) up yet -- measured: on SDL the
         * very first frame reports "no GL context" and a later one probes
         * fine.  Committing to readback on that transient would strand an
         * NVIDIA host on the slow path for the life of the VM, so keep
         * retrying for a short while and only then settle.
         */
        if (++p->probe_tries < NVKVM_PROBE_TRIES) {
            return 0;         /* readback THIS frame; mode stays undecided */
        }
        p->mode = 0;  why = "UI never offered a GL context to probe";
    } else if (nvidia) {
        p->mode = 1;  why = "host UI renders on NVIDIA: native import";
    } else if (linear) {
        /* Unreachable for scanout; see the note above. */
        p->mode = 1;  why = "host UI is cross-vendor but the buffer is linear";
    } else {
        p->mode = 0;
        why = "host UI is cross-vendor and the buffer is block-linear, "
              "which only NVIDIA can import";
    }

    /* Report the mode actually used, and why -- an operator starting a desktop
     * needs to know whether they got zero-copy or paid for a readback. */
    fprintf(stderr,
            "nvkvm present: display mode = %s (%s)%s%s\n",
            p->mode ? "GL zero-copy" : "readback", why,
            p->ui_renderer[0] ? "; host GL renderer: " : "",
            p->ui_renderer[0] ? p->ui_renderer : "");
    fflush(stderr);
    return p->mode;
}


/* Push the pending frame to the GL console with no CPU copy. */
static bool nvkvm_present_gl(NvkvmPresent *p, int fd, uint32_t w, uint32_t h,
                             uint32_t stride, uint32_t fourcc, uint64_t mod)
{
    /* qemu_dmabuf_new takes ownership of fd (freed by qemu_dmabuf_close). */
    QemuDmaBuf *buf = qemu_dmabuf_new(w, h, stride, 0, 0, w, h, fourcc, mod,
                                      fd, false, false);
    if (!buf) {
        close(fd);
        return false;   /* frame never reached the display */
    }
    qemu_console_resize(p->con, w, h);
    dpy_gl_scanout_dmabuf(p->con, buf);
    dpy_gl_update(p->con, 0, 0, w, h);

    /* Retire the previously scanned-out buffer only now that the new one is
     * live (its fd was in use until this point). */
    if (p->cur_buf) {
        /*
         * Tell the UI backend to drop what dpy_gl_scanout_dmabuf() made it
         * create.  That call has the backend glGenTextures() into the
         * QemuDmaBuf; qemu_dmabuf_free() is a bare g_free and does not touch
         * the texture, so without this the texture and its EGLImage are
         * abandoned on every flip.  hw/display/virtio-gpu-udmabuf.c releases
         * here for the same reason; this path did not.
         *
         * Measured cost is small rather than alarming -- a compositor recycles
         * two or three scanout bos, so the abandoned objects alias the same
         * few buffers and cost driver bookkeeping, not VRAM: ~18,000 flips left
         * QEMU's own attributed VRAM flat.  It is still an unbounded per-flip
         * leak of driver-side objects, and it is one call to stop it.
         */
        dpy_gl_release_dmabuf(p->con, p->cur_buf);
        qemu_dmabuf_close(p->cur_buf);
        qemu_dmabuf_free(p->cur_buf);
    }
    for (int i = 0; i < NVKVM_PRESENT_CACHE; i++) {
        if (p->cache[i].valid) {
            glDeleteTextures(1, &p->cache[i].tex);
            eglDestroyImageKHR(qemu_egl_display, p->cache[i].image);
            p->cache[i].valid = false;
        }
    }
    egl_fb_destroy(&p->fb);
    p->cur_buf = buf;
    return true;
}


/* Look the frame's bo up in the import cache, importing it on a miss.
 * Returns the GL texture, or 0.  Never destroys anything on the hot path. */
static GLuint nvkvm_present_cached_tex(NvkvmPresent *p, uint32_t owner,
                                       uint32_t key, int fd,
                                       uint32_t w, uint32_t h, uint32_t stride,
                                       uint32_t fourcc, uint64_t mod)
{
    int free_slot = -1;
    struct stat st;
    /*
     * ino 0 = "could not tell".  Falling back to the old (owner, key, geometry)
     * match is wrong in exactly the recycled-handle case this exists to catch,
     * but re-importing every frame is what SIGSEGV'd libnvidia-eglcore (see the
     * cache comment), so the safe failure is the stale frame, not the crash.
     */
    uint64_t ino = fstat(fd, &st) == 0 ? (uint64_t)st.st_ino : 0;

    for (int i = 0; i < NVKVM_PRESENT_CACHE; i++) {
        if (!p->cache[i].valid) {
            if (free_slot < 0) {
                free_slot = i;
            }
            continue;
        }
        if (p->cache[i].owner == owner && p->cache[i].key == key) {
            /* Same bo id, but the compositor may have reallocated it at a new
             * geometry -- then the cached import describes the wrong memory. */
            if (p->cache[i].ino == ino &&
                p->cache[i].w == w && p->cache[i].h == h &&
                p->cache[i].stride == stride &&
                p->cache[i].fourcc == fourcc && p->cache[i].mod == mod) {
                p->cache[i].used = ++p->tick;
                return p->cache[i].tex;
            }
            if (p->cache[i].ino != ino) {
                /* Recycled GEM handle: same id, different buffer. */
                static unsigned nrecycled;
                if (nrecycled++ < 8) {
                    fprintf(stderr, "nvkvm present: bo %u reused for a new "
                            "dma-buf (ino %llu -> %llu); re-importing\n",
                            key, (unsigned long long)p->cache[i].ino,
                            (unsigned long long)ino);
                }
            }
            glDeleteTextures(1, &p->cache[i].tex);
            eglDestroyImageKHR(qemu_egl_display, p->cache[i].image);
            p->cache[i].valid = false;
            free_slot = i;
            break;
        }
    }

    if (free_slot < 0) {
        /*
         * Full: drop the LEAST RECENTLY USED entry.
         *
         * This used to unconditionally drop slot 0, on the reasoning that "a
         * compositor cycles far fewer bos than this".  That holds for ONE
         * compositor and not for a desktop: every isolate exporting a scanout
         * bo competes for the same eight slots, and short-lived surfaces --
         * tooltips, menus, notifications -- each burn one.  Slot 0 is also the
         * worst possible victim, because it holds the FIRST buffer imported,
         * which is the primary scanout bo: the longest-lived and most
         * frequently used entry in the table.  So once the working set passed
         * eight, every new surface destroyed the primary framebuffer's import,
         * the next frame from it missed and re-imported, and that evicted it
         * again -- steady-state thrash on the one buffer that should never be
         * evicted, with a visible flicker each time it was rebuilt.
         *
         * LRU makes the primary, which is touched every frame, the LAST thing
         * considered rather than the first.
         *
         * Note the churn this avoids is not merely slow: per-frame
         * eglDestroyImageKHR/glDeleteTextures is what reliably SIGSEGV'd
         * libnvidia-eglcore (see the comment on the cache above), so an
         * eviction policy that can thrash is a stability question too.
         */
        int lru = 0;
        for (int i = 1; i < NVKVM_PRESENT_CACHE; i++) {
            if (p->cache[i].used < p->cache[lru].used) {
                lru = i;
            }
        }
        glDeleteTextures(1, &p->cache[lru].tex);
        eglDestroyImageKHR(qemu_egl_display, p->cache[lru].image);
        p->cache[lru].valid = false;
        free_slot = lru;
    }

    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    GLuint tex = nvkvm_import_dmabuf_tex(fd, w, h, stride, fourcc, mod, &image);
    if (!tex) {
        return 0;
    }
    p->cache[free_slot] = (typeof(p->cache[0])){
        .valid = true, .owner = owner, .key = key, .ino = ino, .w = w, .h = h,
        .stride = stride,
        .fourcc = fourcc, .mod = mod, .image = image, .tex = tex,
        .used = ++p->tick,
    };
    return tex;
}

/*
 * NVKVM_PRESENT_SYNC=1 forces the old synchronous readback, for A/B timing.
 * PBOs and glMapBufferRange are GL 3.0 / GLES 3.0; below that, fall back.
 */
static bool nvkvm_pbo_ok(void)
{
    static int ok = -1;
    if (ok < 0) {
        ok = epoxy_gl_version() >= 30 && getenv("NVKVM_PRESENT_SYNC") == NULL;
    }
    return ok;
}

/*
 * Asynchronous readback.
 *
 * egl_fb_read() is a bare glReadPixels into the DisplaySurface: it flushes the
 * pipeline, waits for the GPU to finish, then DMAs w*h*4 bytes across PCIe with
 * the CPU blocked -- and it runs from gfx_update on the main loop with the BQL
 * held, so the guest's vCPUs are stopped for the whole transfer.  That is why
 * this path measured ~3 fps rather than merely "slower than zero-copy": the
 * cost is not the bandwidth, it is stopping the VM to pay it.
 *
 * Screen capture does not work this way.  Read into a pixel buffer object --
 * with one bound, glReadPixels queues a DMA and returns -- and map that buffer
 * on the NEXT frame, by which point the transfer finished long ago and the map
 * does not wait.  One frame of added latency buys back the stall.
 *
 * Returns false when there is nothing to show yet (the first frame, which has
 * only issued its transfer); the caller must not push the surface then, because
 * it still holds whatever was there before.
 */
static bool nvkvm_fb_read_async(NvkvmPresent *p, uint8_t *dst,
                                uint32_t w, uint32_t h)
{
    const size_t sz = (size_t)w * h * 4;

    if (p->pbo_w != w || p->pbo_h != h) {
        if (p->pbo[0]) {
            glDeleteBuffers(2, p->pbo);
            p->pbo[0] = p->pbo[1] = 0;
        }
        glGenBuffers(2, p->pbo);
        for (int i = 0; i < 2; i++) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, p->pbo[i]);
            glBufferData(GL_PIXEL_PACK_BUFFER, sz, NULL, GL_STREAM_READ);
        }
        p->pbo_w = w;
        p->pbo_h = h;
        p->pbo_idx = 0;
        p->pbo_primed = false;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, p->fb.framebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);

    glBindBuffer(GL_PIXEL_PACK_BUFFER, p->pbo[p->pbo_idx]);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, NULL);

    bool have_frame = false;
    const unsigned prev = p->pbo_idx ^ 1u;
    if (p->pbo_primed) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, p->pbo[prev]);
        const void *src = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, sz,
                                           GL_MAP_READ_BIT);
        if (src) {
            memcpy(dst, src, sz);      /* staging is always tightly packed */
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            have_frame = true;
        }
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    p->pbo_idx = prev;
    p->pbo_primed = true;
    return have_frame;
}

/* Size the staging pair for this geometry.  Returns false on OOM. */
static void nvkvm_stage_release(NvkvmPresent *p)
{
    for (unsigned i = 0; i < NVKVM_STAGE_MAX; i++) {
        if (p->stage_buf[i].size) {
            nvkvm_udmabuf_free(&p->stage_buf[i]);   /* also unmaps stage[i] */
            p->stage[i] = NULL;
        } else {
            g_free(p->stage[i]);
            p->stage[i] = NULL;
        }
    }
}

static bool nvkvm_stage_ensure(NvkvmPresent *p, uint32_t w, uint32_t h)
{
    const size_t sz = (size_t)w * h * 4;

    if (p->stage[0] && p->stage_w == w && p->stage_h == h) {
        return true;
    }
    nvkvm_stage_release(p);

    if (p->relay_readback) {
        /*
         * ALL of them must succeed or none is usable: too few buffers hands
         * the compositor one the GPU is still writing into.
         */
        p->stage_n = NVKVM_STAGE_MAX;
        for (unsigned i = 0; i < p->stage_n; i++) {
            if (!nvkvm_udmabuf_alloc(&p->stage_buf[i], sz)) {
                nvkvm_stage_release(p);
                fprintf(stderr, "nvkvm present: could not allocate udmabuf "
                                "staging for %ux%u; the cross-vendor path is "
                                "unavailable\n", w, h);
                return false;
            }
            p->stage[i] = p->stage_buf[i].ptr;
        }
        p->stage_w = w;
        p->stage_h = h;
        p->stage_write = 0;
        p->stage_has_frame = false;
        return true;
    }

    p->stage_n = NVKVM_STAGE_CONSOLE;
    p->stage[0] = g_malloc0(sz);
    p->stage[1] = g_malloc0(sz);
    p->stage_w = w;
    p->stage_h = h;
    p->stage_write = 0;
    p->stage_has_frame = false;
    return p->stage[0] && p->stage[1];
}

/*
 * #125: import the pending frame and read it back into staging.
 *
 * Runs ON THE PRESENT THREAD, where our NVIDIA context is already current and
 * stays current -- so there is no eglMakeCurrent here, and nothing touches the
 * QemuConsole: display ops belong to the main loop and are done by the BH this
 * schedules.  Works regardless of the host's display GPU.
 */
static bool nvkvm_readback_to_stage(NvkvmPresent *p, int fd, uint32_t owner,
                                    uint32_t key,
                                    uint32_t w, uint32_t h, uint32_t stride,
                                    uint32_t fourcc, uint64_t mod)
{
    GLuint tex = nvkvm_present_cached_tex(p, owner, key, fd, w, h, stride,
                                          fourcc, mod);
    if (!tex) {
        close(fd);
        return false;   /* import failed: nothing was shown */
    }
    egl_fb_setup_for_tex(&p->fb, w, h, tex, false);   /* keeps its FBO */

    if (!nvkvm_stage_ensure(p, w, h)) {
        close(fd);
        return false;   /* out of memory: nothing was shown */
    }
    uint8_t *dst = p->stage[p->stage_write];

    bool have_frame;
    if (nvkvm_pbo_ok()) {
        have_frame = nvkvm_fb_read_async(p, dst, w, h);
    } else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, p->fb.framebuffer);
        glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
        glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, dst);
        have_frame = true;
    }

    if (have_frame) {
        /*
         * Publish: the buffer just filled becomes the one the main loop shows,
         * and the thread moves to the other one.  Two buffers are enough
         * because the main loop consumes a frame before we can come back
         * around to the buffer it is showing.
         */
        pthread_mutex_lock(&p->lock);
        p->stage_ready     = p->stage_write;
        p->stage_has_frame = true;
        p->stage_write     = (p->stage_write + 1u) % p->stage_n;
        pthread_mutex_unlock(&p->lock);
    }
    close(fd);                             /* readback owns + consumes the fd */
    /* The first frame only issues its PBO transfer, so it legitimately shows
     * nothing yet; that is a pipeline warm-up, not a failure to display. */
    return have_frame;
}

/*
 * S-4: drop the cached imports of isolates that have died.  PRESENT THREAD
 * only -- eglDestroyImageKHR/glDeleteTextures need our context current, and
 * since #125 that context lives on the present thread, which is also why this
 * cannot run where the death is noticed.
 *
 * Until this existed nothing ever invalidated on isolate death, so a dead
 * compositor's EGLImage held its dma-buf -- and the VRAM behind it -- pinned
 * for the life of the VM, and its cache slot went on answering for whatever
 * bo id the next isolate happened to reuse.
 *
 * p->cur_buf (the GL zero-copy path's live scanout) is deliberately left
 * alone: it is one most-recent frame rather than an accumulating pin, and
 * pulling it out from under the UI mid-scanout is a worse failure than showing
 * a stale frame until the next flip replaces it.
 */
static void nvkvm_present_reap_dead(NvkvmPresent *p)
{
    uint32_t dead[16];
    unsigned ndead;
    bool     all, any = false;

    pthread_mutex_lock(&p->lock);
    ndead = p->ndead;
    all   = p->dead_all;
    memcpy(dead, p->dead, ndead * sizeof(dead[0]));
    p->ndead   = 0;
    p->dead_all = false;
    pthread_mutex_unlock(&p->lock);

    if (!ndead && !all) {
        return;
    }
    for (int i = 0; i < NVKVM_PRESENT_CACHE; i++) {
        any = any || p->cache[i].valid;
    }
    if (!any) {
        return;   /* nothing imported yet -- EGL may not even be up */
    }
    bool dropped = false;
    for (int i = 0; i < NVKVM_PRESENT_CACHE; i++) {
        if (!p->cache[i].valid) {
            continue;
        }
        bool drop = all;
        for (unsigned k = 0; !drop && k < ndead; k++) {
            drop = (p->cache[i].owner == dead[k]);
        }
        if (!drop) {
            continue;
        }
        glDeleteTextures(1, &p->cache[i].tex);
        eglDestroyImageKHR(qemu_egl_display, p->cache[i].image);
        p->cache[i].valid = false;
        dropped = true;
    }
    if (dropped) {
        /* The FBO may still name a texture we just deleted; the next frame
         * re-runs egl_fb_setup_for_tex anyway. */
        egl_fb_destroy(&p->fb);
    }
}

/*
 * Present-path frame counters (NVKVM_PRESENT_TIMING=1).
 *
 * A rate must come from a COUNTER, never from counting log lines: only some
 * paths emit a line per frame, which is how a phantom "21 presents/s ceiling"
 * was once measured on a pipeline that was in fact running at a full 60.
 * These count every frame the display consumed and every frame the
 * single-slot handoff had to drop, and print ONE line per wall-clock second
 * carrying the rate itself -- so the number in the log is already frames/s and
 * needs no arithmetic over the log, which is where the phantom came from.
 *
 * Off unless the environment variable is set, so a normal run pays nothing.
 */
static unsigned nvkvm_st_consumed, nvkvm_st_dropped;
/*
 * Whether the frame actually reached the display.  This is not pedantry: with
 * EGL unavailable the present path returns at its first line, and the old code
 * counted that as a presented frame -- "60.0 frames/s dropped=0" printed once a
 * second at a blank screen, with the one line that said why scrolled past at
 * startup.  Stats that say healthy while nothing is displayed are worse than no
 * stats, because someone acts on them.
 */
static unsigned nvkvm_st_failed, nvkvm_st_bucket_failed;
static unsigned nvkvm_st_bucket_consumed, nvkvm_st_bucket_dropped;
static double   nvkvm_st_present_us, nvkvm_st_bucket_us;
static double   nvkvm_st_bucket_t0;
static bool     nvkvm_st_on, nvkvm_st_checked;

static bool nvkvm_disp_stats(void)
{
    if (!nvkvm_st_checked) {
        nvkvm_st_checked = true;
        nvkvm_st_on = getenv("NVKVM_PRESENT_TIMING") != NULL;
    }
    return nvkvm_st_on;
}

static double nvkvm_st_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* One consumed frame, `us` spent presenting it.  Called from the main loop on
 * the GL path and from the present thread on the readback path -- never both
 * in one run, since the mode is decided once. */
static void nvkvm_st_frame(double us, bool shown)
{
    double now;

    if (shown) {
        nvkvm_st_present_us += us;
        nvkvm_st_bucket_us  += us;
        nvkvm_st_consumed++;
        nvkvm_st_bucket_consumed++;
    } else {
        nvkvm_st_failed++;
        nvkvm_st_bucket_failed++;
    }

    now = nvkvm_st_now();
    if (nvkvm_st_bucket_t0 == 0.0) {
        nvkvm_st_bucket_t0 = now;
        return;
    }
    if (now - nvkvm_st_bucket_t0 >= 1.0) {
        fprintf(stderr,
                "nvkvm disp stats: %.1f frames/s dropped=%u failed=%u "
                "present_mean=%.2fms (total consumed=%u dropped=%u "
                "failed=%u)%s\n",
                nvkvm_st_bucket_consumed / (now - nvkvm_st_bucket_t0),
                nvkvm_st_bucket_dropped, nvkvm_st_bucket_failed,
                nvkvm_st_bucket_consumed
                    ? nvkvm_st_bucket_us / nvkvm_st_bucket_consumed / 1000.0
                    : 0.0,
                nvkvm_st_consumed, nvkvm_st_dropped, nvkvm_st_failed,
                (nvkvm_st_consumed == 0 && nvkvm_st_failed > 0)
                    ? "  <-- NOTHING IS BEING DISPLAYED" : "");
        fflush(stderr);
        nvkvm_st_bucket_t0       = now;
        nvkvm_st_bucket_consumed = 0;
        nvkvm_st_bucket_dropped  = 0;
        nvkvm_st_bucket_failed   = 0;
        nvkvm_st_bucket_us       = 0.0;
    }
}

/*
 * #125: the present thread.
 *
 * Owns the NVIDIA EGL context for the life of the VM.  egl_init() makes the
 * context current on the calling thread, so calling it HERE -- and only here --
 * is what pins it to this thread; it is never made current anywhere else and
 * never unbound, which is the whole point.  The UI keeps its own context on the
 * main loop and the two never meet.
 */
static void *nvkvm_present_thread_fn(void *opaque)
{
    NvkvmPresent *p = opaque;

    /*
     * The display, config and context were created on the main loop under the
     * BQL (see nvkvm_present_thread_start) -- egl_init() writes QEMU-global
     * state (qemu_egl_display, qemu_egl_rn_ctx, qemu_egl_rn_gbm_dev) that the
     * UI backend also reads, so it must not race the main loop.  All that is
     * left here is to BIND that context to this thread, once, forever.
     */
    /*
     * Diagnose BEFORE calling: a bare eglGetError() here is not trustworthy.
     * Measured on an SDL host (2026-08-23): this path reported a bind failure
     * with eglGetError() == 0x3000, which is EGL_SUCCESS -- so the error code
     * pointed at nothing and the operator was left with a black window and no
     * usable message.  eglGetError() is per-thread and is cleared by a
     * preceding successful call, so it can legitimately read SUCCESS after a
     * false return.  Check the preconditions explicitly instead.
     */
    if (qemu_egl_display == EGL_NO_DISPLAY || qemu_egl_rn_ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "nvkvm present: no render-node EGL context to bind "
                        "(display=%p ctx=%p) -- egl_init() did not run or did "
                        "not complete; nothing will be displayed\n",
                        (void *)qemu_egl_display, (void *)qemu_egl_rn_ctx);
        qatomic_set(&p->thread_failed, true);
        return NULL;
    }
    if (!eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        qemu_egl_rn_ctx)) {
        EGLint err = eglGetError();
        fprintf(stderr, "nvkvm present: could not bind the render-node context "
                        "to the present thread (eglGetError=0x%04x%s); nothing "
                        "will be displayed\n", (unsigned)err,
                        err == EGL_BAD_ACCESS
                            ? " EGL_BAD_ACCESS -- the context is already current"
                              " on another thread; see nvkvm_present_capture()"
                            : err == EGL_SUCCESS
                            ? " EGL_SUCCESS -- the driver returned failure"
                              " without setting an error, so this code is not"
                              " the real cause; check whether another thread"
                              " holds the context"
                            : "");
        qatomic_set(&p->thread_failed, true);
        return NULL;
    }

    for (;;) {
        qemu_sem_wait(&p->wake);
        if (qatomic_read(&p->thread_stop)) {
            break;
        }

        nvkvm_present_reap_dead(p);   /* S-4; our context is current here */

        pthread_mutex_lock(&p->lock);
        if (!p->dirty || p->fd < 0) {
            pthread_mutex_unlock(&p->lock);
            continue;
        }
        int      fd     = p->fd;
        uint32_t w      = p->w,  h = p->h, stride = p->stride;
        uint32_t fourcc = p->fourcc;
        uint64_t mod    = p->modifier;
        uint32_t key    = p->key;
        uint32_t owner  = p->owner;
        p->fd    = -1;
        p->dirty = false;
        pthread_mutex_unlock(&p->lock);

        struct timespec ta, tb;
        bool stats = nvkvm_disp_stats();
        if (stats) {
            clock_gettime(CLOCK_MONOTONIC, &ta);
        }

        bool shown = nvkvm_readback_to_stage(p, fd, owner, key, w, h, stride,
                                             fourcc, mod);

        if (stats) {
            clock_gettime(CLOCK_MONOTONIC, &tb);
            nvkvm_st_frame((tb.tv_sec - ta.tv_sec) * 1e6 +
                           (tb.tv_nsec - ta.tv_nsec) / 1e3, shown);
        }

        /* Ask the main loop to show it (display ops are main-loop only). */
        qemu_bh_schedule(p->bh);
    }
    return NULL;
}

/* Main loop, BQL held.  Bring EGL up here, then hand the context to the
 * thread; see nvkvm_present_thread_fn for why the split. */
static bool nvkvm_present_thread_start(NvkvmPresent *p)
{
    if (qatomic_read(&p->thread_failed)) {
        return false;
    }
    if (p->thread_started) {
        return true;
    }
    if (!nvkvm_present_egl_ensure()) {
        qatomic_set(&p->thread_failed, true);
        return false;
    }
    /* egl_init() left the context current on THIS thread; release it so the
     * present thread can take it.  A context is current on one thread at a
     * time, and leaving it bound here is precisely the collision with the UI
     * backend that this whole change exists to remove. */
    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    p->thread_started = true;
    qemu_thread_create(&p->thread, "nvkvm-present", nvkvm_present_thread_fn,
                       p, QEMU_THREAD_JOINABLE);
    return true;
}

/*
 * Show whatever the present thread has finished, on the main loop.
 *
 * One memcpy into the console's own surface -- deliberately NOT a new
 * DisplaySurface per frame.  Wrapping each staging buffer with
 * qemu_create_displaysurface_from() and calling dpy_gfx_replace_surface()
 * would avoid this copy, but replacing the surface makes the UI backend
 * rebuild its texture every frame (ui/sdl2-gl.c: sdl2_gl_switch() does
 * surface_gl_destroy_texture + surface_gl_create_texture), and per-frame
 * GL object churn on this path is exactly what killed the NVIDIA driver
 * before -- see the import-cache note above.  A 1080p copy is ~1 ms and
 * buys a stable texture; the work worth moving off the main loop (the
 * import, the GPU readback and the PBO map) is already on the thread.
 *
 * qemu_console_resize() early-returns when the geometry is unchanged, so it
 * costs nothing per frame and only builds a surface when the mode changes.
 */
static void nvkvm_present_publish(NvkvmPresent *p)
{
    pthread_mutex_lock(&p->lock);
    bool     have = p->stage_has_frame;
    unsigned idx  = p->stage_ready;
    uint32_t w    = p->stage_w, h = p->stage_h;
    p->stage_has_frame = false;
    pthread_mutex_unlock(&p->lock);

    if (!have || !p->stage[idx]) {
        return;
    }

    qemu_console_resize(p->con, (int)w, (int)h);
    DisplaySurface *ds = qemu_console_surface(p->con);
    if (!ds || surface_width(ds) != (int)w || surface_height(ds) != (int)h) {
        fprintf(stderr, "nvkvm present: console surface unusable for %ux%u\n",
                w, h);
        return;
    }

    const uint8_t *src        = p->stage[idx];
    const size_t   dst_stride = surface_stride(ds);
    if (dst_stride == (size_t)w * 4) {
        memcpy(surface_data(ds), src, (size_t)w * h * 4);
    } else {
        for (uint32_t y = 0; y < h; y++) {
            memcpy((uint8_t *)surface_data(ds) + y * dst_stride,
                   src + (size_t)y * w * 4, (size_t)w * 4);
        }
    }

    /*
     * NVKVM_PRESENT_DUMP=<path>: write what the window is about to show, as a
     * PPM.  On a headless host, or one whose compositor refuses screenshots,
     * this is the only way to answer "is there a picture" without asking a
     * human to look at the screen -- and a frame counter cannot answer it.
     */
    {
        const char *dump = getenv("NVKVM_PRESENT_DUMP");
        if (dump) {
            static unsigned dn;
            if ((dn++ % 120) == 0) {
                nvkvm_write_ppm(dump, ds);
            }
        }
    }
    dpy_gfx_update(p->con, 0, 0, (int)w, (int)h);
}

/* GraphicHwOps.gfx_update — main loop, BQL held. */
static void nvkvm_present_gfx_update(void *opaque)
{
    NvkvmPresent *p = opaque;

    /* Readback path: the thread did the work; show the result. */
    nvkvm_present_publish(p);

    pthread_mutex_lock(&p->lock);
    bool     pending = (p->dirty && p->fd >= 0);
    uint64_t mod     = p->modifier;
    pthread_mutex_unlock(&p->lock);

    if (!pending) {
        return;
    }

    if (nvkvm_present_decide_mode(p, mod) == 1) {
        /*
         * GL zero-copy: hands the dma-buf straight to the UI, so it needs the
         * UI's context and none of ours -- it belongs on the main loop and
         * stays here.
         */
        pthread_mutex_lock(&p->lock);
        if (!p->dirty || p->fd < 0) {
            pthread_mutex_unlock(&p->lock);
            return;
        }
        int      fd     = p->fd;
        uint32_t w      = p->w, h = p->h, stride = p->stride;
        uint32_t fourcc = p->fourcc;
        uint64_t gmod   = p->modifier;
        p->fd    = -1;
        p->dirty = false;
        pthread_mutex_unlock(&p->lock);

        struct timespec ta, tb;
        bool stats = nvkvm_disp_stats();
        if (stats) {
            clock_gettime(CLOCK_MONOTONIC, &ta);
        }
        bool shown = nvkvm_present_gl(p, fd, w, h, stride, fourcc, gmod);
        if (stats) {
            clock_gettime(CLOCK_MONOTONIC, &tb);
            nvkvm_st_frame((tb.tv_sec - ta.tv_sec) * 1e6 +
                           (tb.tv_nsec - ta.tv_nsec) / 1e3, shown);
        }
    } else {
        /* Readback: wake the present thread, which drains the slot itself. */
        if (nvkvm_present_thread_start(p)) {
            qemu_sem_post(&p->wake);
        }
    }
}

static void nvkvm_present_invalidate(void *opaque)
{
    (void)opaque;            /* full redraw happens on the next flip */
}

/*
 * GraphicHwOps.ui_info -- the host window's geometry, on its way to the guest.
 *
 * Its ABSENCE was the bug.  Without this callback dpy_ui_info_supported() is
 * false and QEMU silently has no channel to tell the guest the window changed,
 * so the guest goes on scanning out whatever size it booted at and the host
 * resamples it into the window.  That is why enlarging the window made the
 * picture softer instead of sharper, and it is half of why the guest's buffer
 * can never cover the host output and so can never reach a hardware plane.
 *
 * Advisory in both directions: we forward the size and the guest's own
 * compositor decides whether to re-mode.  A window is a host gesture; a mode
 * is the guest's business.
 */
static void nvkvm_present_ui_info(void *opaque, uint32_t head,
                                  QemuUIInfo *info)
{
    NvkvmPresent *p = opaque;

    (void)head;                 /* one head */
    if (!p || !info) {
        return;
    }
    nvkvm_virtio_push_ui_info(p->nv, info->width, info->height,
                              info->refresh_rate);
}

static const GraphicHwOps nvkvm_present_hwops = {
    .invalidate  = nvkvm_present_invalidate,
    .gfx_update  = nvkvm_present_gfx_update,
    .ui_info     = nvkvm_present_ui_info,
};

/* Bottom half: runs on the main loop, pokes the display to drain the slot
 * promptly (lower latency than waiting for the periodic refresh tick). */
static void nvkvm_present_bh(void *opaque)
{
    NvkvmPresent *p = opaque;

    /*
     * Main loop, BQL held -- which is exactly why the submit happens HERE and
     * not on the present thread.  The relay socket is BQL-owned (see the
     * ownership contract in nvkvm_display_relay.h); a second owner is the
     * RR-03 defect, and the present thread would be one.
     */
    if (p->relay_readback) {
        unsigned idx;
        bool have;
        uint32_t w, h;

        pthread_mutex_lock(&p->lock);
        have = p->stage_has_frame;
        idx  = p->stage_ready;
        w    = p->stage_w;
        h    = p->stage_h;
        pthread_mutex_unlock(&p->lock);

        if (have && p->stage_buf[idx].size) {
            /*
             * TIER 3.  If the display has told us it cannot take even
             * XR24 + LINEAR as a dma-buf -- which happens when it advertised
             * the modifier and then refused the import -- fall to the SAME
             * pages as a plain memfd.  wl_shm is a core Wayland global, so a
             * compositor cannot refuse it for want of import support; it just
             * pays an upload per frame.
             *
             * udmabuf gives us both handles to one allocation, so this costs
             * nothing but choosing a different fd.
             */
            bool shm = nvkvm_display_relay_format_verdict(DRM_FORMAT_XR24_LOCAL,
                                                          0) == 0;
            /*
             * dup(): relay_submit takes ownership of the fd it is given and
             * retains it as the last frame, but ours belongs to the staging
             * set and is reused every frame.
             */
            int fd = dup(shm ? p->stage_buf[idx].memfd
                             : p->stage_buf[idx].dmabuf);

            if (shm) {
                static bool told;

                if (!told) {
                    told = true;
                    fprintf(stderr, "nvkvm present: the display refused a "
                            "LINEAR dma-buf as well, so frames go over wl_shm "
                            "-- the same pages, shared as plain memory.  Works "
                            "on any compositor; costs it one upload a frame.\n");
                }
            }

            if (fd < 0) {
                fprintf(stderr, "nvkvm present: dup of the staging dma-buf "
                                "failed: %s\n", strerror(errno));
                return;
            }
            /*
             * XR24, not the guest's fourcc: the readback packs BGRA and the
             * buffer is opaque, and XR24 is what every compositor advertises
             * against LINEAR.  Stride is tight -- glReadPixels was told so.
             */
            if (!nvkvm_display_relay_submit_flags(p->nv, fd, w, h, w * 4,
                                                  DRM_FORMAT_XR24_LOCAL, 0,
                                                  shm)) {
                close(fd);
            }
        }
        return;
    }

    graphic_hw_update(p->con);
}

int nvkvm_present_console_init(struct DeviceState *dev, struct VirtIONvgpu *nv)
{
    NvkvmPresent *p = g_new0(NvkvmPresent, 1);
    pthread_mutex_init(&p->lock, NULL);
    p->fd   = -1;
    p->mode = -1;
    p->dev  = dev;
    p->nv   = nv;
    p->con  = graphic_console_init(dev, 0, &nvkvm_present_hwops, p);
    p->bh   = qemu_bh_new(nvkvm_present_bh, p);
    qemu_sem_init(&p->wake, 0);   /* #125: present thread starts on first use */
    nv->present_ctx = p;
    fprintf(stderr, "nvkvm present: registered QemuConsole for guest GPU "
            "scanout (window display)\n");
    return 0;
}

void nvkvm_present_console_set_device(struct VirtIONvgpu *nv,
                                      struct DeviceState *dev)
{
    NvkvmPresent *p = nv->present_ctx;

    if (!p || !p->con || !dev) {
        return;
    }
    /* See the header for why this is needed.  Same call, same reason, as
     * virtio_gpu_pci_base_realize(). */
    object_property_set_link(OBJECT(p->con), "device", OBJECT(dev),
                             &error_abort);
}

void nvkvm_present_console_fini(struct VirtIONvgpu *nv)
{
    NvkvmPresent *p = nv->present_ctx;
    if (!p) {
        return;
    }
    nv->present_ctx = NULL;
    /* #125: stop the present thread before tearing anything it touches down. */
    if (p->thread_started) {
        qatomic_set(&p->thread_stop, true);
        qemu_sem_post(&p->wake);
        qemu_thread_join(&p->thread);
    }
    qemu_sem_destroy(&p->wake);
    nvkvm_stage_release(p);   /* g_free or munmap+close, per how it was made */
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
    if (p->fd >= 0) {
        close(p->fd);
    }
    pthread_mutex_destroy(&p->lock);
    g_free(p);
}

bool nvkvm_present_submit(struct VirtIONvgpu *nv, int dmabuf_fd,
                          uint32_t owner_isolate_id, uint32_t buf_key,
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
        if (nvkvm_disp_stats()) {
            nvkvm_st_dropped++;
            nvkvm_st_bucket_dropped++;
        }
    }
    p->fd       = dmabuf_fd;
    p->owner    = owner_isolate_id;   /* S-4: half of the cache identity */
    p->key      = buf_key;
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

/*
 * Cross-vendor present: detile the guest's frame with the guest's own GPU and
 * hand the broker a LINEAR buffer instead.
 *
 * Called when the broker has answered CMD_QUERY_FORMAT with "cannot show
 * that" -- which on a hybrid host is the only possible answer, since the
 * compositor's GPU cannot read NVIDIA tiling at any price.
 *
 * Deliberately NOT scheduling the BH here the way nvkvm_present_submit() does:
 * there is nothing staged yet.  The present thread schedules it once a frame is
 * ready, and the BH is what submits to the relay -- see nvkvm_present_bh() for
 * why the submit must happen there and not on the thread.
 *
 * Returns false if the frame could not be taken; the caller still owns the fd.
 */
bool nvkvm_present_submit_readback(struct VirtIONvgpu *nv, int dmabuf_fd,
                                   uint32_t owner_isolate_id, uint32_t buf_key,
                                   uint32_t width, uint32_t height,
                                   uint32_t stride, uint32_t fourcc,
                                   uint64_t modifier)
{
    NvkvmPresent *p = nv->present_ctx;

    if (!p) {
        return false;
    }
    if (!p->relay_readback) {
        if (!nvkvm_udmabuf_available()) {
            return false;     /* said why once, at init; caller falls back */
        }
        /*
         * Latched, not re-decided per frame.  The mode probe is bypassed
         * outright: this path exists precisely because zero-copy is impossible
         * here, so there is nothing to probe for.
         */
        p->relay_readback = true;
        p->mode = 0;
        fprintf(stderr, "nvkvm present: the display cannot import the guest's "
                        "buffers, so frames are detiled by the guest's GPU into "
                        "a LINEAR udmabuf the display can take (one GPU "
                        "transfer, no CPU copy)\n");
    }

    pthread_mutex_lock(&p->lock);
    if (p->fd >= 0) {
        close(p->fd);        /* drop the frame the thread has not taken yet */
        if (nvkvm_disp_stats()) {
            nvkvm_st_dropped++;
            nvkvm_st_bucket_dropped++;
        }
    }
    p->fd       = dmabuf_fd;
    p->owner    = owner_isolate_id;
    p->key      = buf_key;
    p->w        = width;
    p->h        = height;
    p->stride   = stride;
    p->fourcc   = fourcc;
    p->modifier = modifier;
    p->dirty    = true;
    pthread_mutex_unlock(&p->lock);

    if (!nvkvm_present_thread_start(p)) {
        pthread_mutex_lock(&p->lock);
        p->fd    = -1;       /* give the fd back rather than leak it */
        p->dirty = false;
        pthread_mutex_unlock(&p->lock);
        return false;
    }
    qemu_sem_post(&p->wake);
    return true;
}

void nvkvm_present_forget_isolate(struct VirtIONvgpu *nv, uint32_t isolate_id)
{
    NvkvmPresent *p = nv->present_ctx;
    if (!p || isolate_id == 0) {
        return;
    }

    pthread_mutex_lock(&p->lock);
    /*
     * A frame this isolate submitted that the display has not drained yet
     * describes memory whose owner is gone -- retire it here rather than
     * import it after the fact.
     */
    if (p->fd >= 0 && p->owner == isolate_id) {
        close(p->fd);
        p->fd    = -1;
        p->dirty = false;
    }
    if (p->ndead < ARRAY_SIZE(p->dead)) {
        p->dead[p->ndead++] = isolate_id;
    } else {
        p->dead_all = true;   /* rather over-forget than keep a dead import */
    }
    pthread_mutex_unlock(&p->lock);

    if (p->bh) {
        qemu_bh_schedule(p->bh);   /* reaped on the main loop, see gfx_update */
    }
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
void nvkvm_present_console_set_device(struct VirtIONvgpu *nv,
                                      struct DeviceState *dev)
{
    (void)nv; (void)dev;
}
bool nvkvm_present_submit(struct VirtIONvgpu *nv, int dmabuf_fd,
                          uint32_t owner_isolate_id, uint32_t buf_key,
                          uint32_t width, uint32_t height, uint32_t stride,
                          uint32_t fourcc, uint64_t modifier)
{
    (void)nv; (void)dmabuf_fd; (void)owner_isolate_id; (void)buf_key;
    (void)width; (void)height;
    (void)stride; (void)fourcc; (void)modifier;
    return false;            /* not accepted → caller closes the fd */
}
/*
 * The cross-vendor path needs EGL by construction -- only the guest's GPU can
 * detile its own tiling -- so in a no-OpenGL build it does not exist.  A
 * compute-only VMM has no display to be cross-vendor with.
 */
bool nvkvm_present_submit_readback(struct VirtIONvgpu *nv, int dmabuf_fd,
                                   uint32_t owner_isolate_id, uint32_t buf_key,
                                   uint32_t width, uint32_t height,
                                   uint32_t stride, uint32_t fourcc,
                                   uint64_t modifier)
{
    (void)nv; (void)dmabuf_fd; (void)owner_isolate_id; (void)buf_key;
    (void)width; (void)height;
    (void)stride; (void)fourcc; (void)modifier;
    return false;
}
void nvkvm_present_forget_isolate(struct VirtIONvgpu *nv, uint32_t isolate_id)
{
    (void)nv; (void)isolate_id;
}
#endif
