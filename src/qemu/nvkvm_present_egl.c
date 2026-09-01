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

#define NVKVM_PRESENT_CACHE 8

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

    /* No cached DisplaySurface: the console owns it and may free it at any
     * time (see nvkvm_present_readback).  Fetch it per frame. */

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
}


/* Look the frame's bo up in the import cache, importing it on a miss.
 * Returns the GL texture, or 0.  Never destroys anything on the hot path. */
static GLuint nvkvm_present_cached_tex(NvkvmPresent *p, uint32_t owner,
                                       uint32_t key, int fd,
                                       uint32_t w, uint32_t h, uint32_t stride,
                                       uint32_t fourcc, uint64_t mod)
{
    int free_slot = -1;

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
            if (p->cache[i].w == w && p->cache[i].h == h &&
                p->cache[i].stride == stride &&
                p->cache[i].fourcc == fourcc && p->cache[i].mod == mod) {
                p->cache[i].used = ++p->tick;
                return p->cache[i].tex;
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
        .valid = true, .owner = owner, .key = key, .w = w, .h = h,
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
static bool nvkvm_fb_read_async(NvkvmPresent *p, DisplaySurface *ds,
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
            const size_t dst_stride = surface_stride(ds);
            if (dst_stride == (size_t)w * 4) {
                memcpy(surface_data(ds), src, sz);
            } else {
                for (uint32_t y = 0; y < h; y++) {
                    memcpy((uint8_t *)surface_data(ds) + y * dst_stride,
                           (const uint8_t *)src + (size_t)y * w * 4,
                           (size_t)w * 4);
                }
            }
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            have_frame = true;
        }
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    p->pbo_idx = prev;
    p->pbo_primed = true;
    return have_frame;
}

/* Import the pending frame on our private NVIDIA EGL context, read it back to a
 * CPU surface, and push it to the 2D display.  Works for any host display GPU. */
static void nvkvm_present_readback(NvkvmPresent *p, int fd, uint32_t owner,
                                   uint32_t key,
                                   uint32_t w, uint32_t h, uint32_t stride,
                                   uint32_t fourcc, uint64_t mod)
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

    GLuint tex = nvkvm_present_cached_tex(p, owner, key, fd, w, h, stride,
                                          fourcc, mod);
    if (!tex) {
        goto out_ctx;
    }
    egl_fb_setup_for_tex(&p->fb, w, h, tex, false);   /* keeps its FBO */

    /*
     * Never cache the DisplaySurface.  The console OWNS it: qemu_console_resize()
     * creates and installs one itself (ui/console.c), and every
     * dpy_gfx_replace_surface() frees the outgoing surface.  Ask the console
     * for its current surface each frame rather than holding a pointer that a
     * window resize or VM reset can free under us -- the glReadPixels below
     * writes w*h*4 bytes (8 MB at 1080p) into whatever it is given.
     */
    qemu_console_resize(p->con, w, h);
    DisplaySurface *ds = qemu_console_surface(p->con);
    if (!ds || surface_width(ds) != (int)w || surface_height(ds) != (int)h) {
        fprintf(stderr, "nvkvm present: console surface unusable for %ux%u\n",
                w, h);
        goto out_ctx;
    }
    bool have_frame;
    if (nvkvm_pbo_ok()) {
        have_frame = nvkvm_fb_read_async(p, ds, w, h);
    } else {
        egl_fb_read(ds, &p->fb);           /* glReadPixels texture -> CPU BGRA */
        have_frame = true;
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
    if (have_frame) {
        dpy_gfx_update(p->con, 0, 0, w, h);
    }

out_ctx:
    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    close(fd);                             /* readback owns + consumes the fd */
}

/*
 * S-4: drop the cached imports of isolates that have died.  Main loop only --
 * eglDestroyImageKHR/glDeleteTextures need our context current, which is also
 * why this cannot run where the death is noticed.
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
    if (!eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        qemu_egl_rn_ctx)) {
        return;
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
    eglMakeCurrent(qemu_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
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

/* GraphicHwOps.gfx_update — main loop, BQL held.  Drain the slot and present. */
static void nvkvm_present_gfx_update(void *opaque)
{
    NvkvmPresent *p = opaque;

    nvkvm_present_reap_dead(p);      /* S-4, before anything is presented */

    pthread_mutex_lock(&p->lock);
    if (!p->dirty || p->fd < 0) {
        pthread_mutex_unlock(&p->lock);
        return;
    }
    int      fd     = p->fd;
    uint32_t w      = p->w,  h = p->h, stride = p->stride, fourcc = p->fourcc;
    uint64_t mod    = p->modifier;
    uint32_t key    = p->key;
    uint32_t owner  = p->owner;
    p->fd    = -1;            /* take ownership of this frame's fd */
    p->dirty = false;
    pthread_mutex_unlock(&p->lock);

    struct timespec ta, tb;
    bool stats = nvkvm_disp_stats();
    if (stats) {
        clock_gettime(CLOCK_MONOTONIC, &ta);
    }

    if (nvkvm_present_decide_mode(p) == 1) {
        nvkvm_present_gl(p, fd, w, h, stride, fourcc, mod);
    } else {
        nvkvm_present_readback(p, fd, owner, key, w, h, stride, fourcc, mod);
    }

    if (stats) {
        double now, us;

        clock_gettime(CLOCK_MONOTONIC, &tb);
        us = (tb.tv_sec - ta.tv_sec) * 1e6 + (tb.tv_nsec - ta.tv_nsec) / 1e3;
        nvkvm_st_present_us += us;
        nvkvm_st_bucket_us  += us;
        nvkvm_st_consumed++;
        nvkvm_st_bucket_consumed++;

        now = nvkvm_st_now();
        if (nvkvm_st_bucket_t0 == 0.0) {
            nvkvm_st_bucket_t0 = now;
        } else if (now - nvkvm_st_bucket_t0 >= 1.0) {
            fprintf(stderr,
                    "nvkvm disp stats: %.1f frames/s dropped=%u "
                    "present_mean=%.2fms (total consumed=%u dropped=%u)\n",
                    nvkvm_st_bucket_consumed / (now - nvkvm_st_bucket_t0),
                    nvkvm_st_bucket_dropped,
                    nvkvm_st_bucket_consumed
                        ? nvkvm_st_bucket_us / nvkvm_st_bucket_consumed / 1000.0
                        : 0.0,
                    nvkvm_st_consumed, nvkvm_st_dropped);
            fflush(stderr);
            nvkvm_st_bucket_t0       = now;
            nvkvm_st_bucket_consumed = 0;
            nvkvm_st_bucket_dropped  = 0;
            nvkvm_st_bucket_us       = 0.0;
        }
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
void nvkvm_present_forget_isolate(struct VirtIONvgpu *nv, uint32_t isolate_id)
{
    (void)nv; (void)isolate_id;
}
#endif
