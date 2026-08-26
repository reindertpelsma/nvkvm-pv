/*
 * REPRODUCER: NVIDIA advertises DRM_FORMAT_MOD_LINEAR as importable and then
 * refuses to import it.
 *
 * MEASURED on an RTX 4070, driver 595.84, 2026-08-26:
 *
 *   explicit block-linear 0x0300000000606014  -> binds, FBO-complete
 *   explicit LINEAR 0x0, NVIDIA's own GBM     -> GL_INVALID_OPERATION on bind
 *   explicit LINEAR 0x0, AMD-allocated        -> GL_INVALID_OPERATION on bind
 *   implicit (no modifier attributes)         -> fails for EVERYTHING, incl.
 *                                                block-linear, so the implicit
 *                                                path says nothing about LINEAR
 *
 * while nvidia_advertised_modifiers.c shows eglQueryDmaBufModifiersEXT listing
 * 0x0000000000000000 among the 13 modifiers it claims for XRGB8888.  Advertise
 * and implement disagree.
 *
 * WHY IT MATTERS HERE.  Compositors build their zwp_linux_dmabuf_v1 list from
 * that same query, so GNOME/Mutter on NVIDIA advertises XR24+LINEAR and then
 * answers the import with "Could not bind the given EGLImage to a
 * CoglTexture2D" -- a WAYLAND PROTOCOL ERROR, which kills the client.  That is
 * what the broker's first-use probe (src/broker/nb_session_wl.c, `proven`)
 * exists to survive, and it is why "the display advertises it" is not a safe
 * basis for choosing a present path.
 *
 * NOTE the two are independent: NVIDIA has never refused to WRITE linear
 * memory.  The cross-vendor present path relies on exactly that -- glReadPixels
 * into a udmabuf mmap, which the iGPU then imports as a LINEAR dma-buf.
 *
 * Build: gcc -O1 -o t nvidia_linear_dmabuf_import.c \
 *            $(pkg-config --cflags --libs gbm egl libdrm) -lGL
 * Needs two GPUs for the AMD case; it skips what it cannot allocate.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <drm_fourcc.h>

#define W 256
#define H 256

static PFNEGLCREATEIMAGEKHRPROC          pCreateImage;
static PFNEGLDESTROYIMAGEKHRPROC         pDestroyImage;
static PFNEGLGETPLATFORMDISPLAYEXTPROC   pGetPlatformDisplay;
static void (*pEGLImageTargetTexture2D)(GLenum, GLeglImageOES);
static void (*pCopyImageSubData)(GLuint,GLenum,GLint,GLint,GLint,GLint,
                                 GLuint,GLenum,GLint,GLint,GLint,GLint,
                                 GLsizei,GLsizei,GLsizei);
static void (*pGenFramebuffers)(GLsizei, GLuint*);
static void (*pBindFramebuffer)(GLenum, GLuint);
static void (*pFramebufferTexture2D)(GLenum,GLenum,GLenum,GLuint,GLint);
static GLenum (*pCheckFramebufferStatus)(GLenum);

struct buf { struct gbm_bo *bo; int fd; uint32_t stride; uint64_t mod; };

static int alloc_linear(const char *label, const char *node, struct buf *out)
{
    int dfd = open(node, O_RDWR | O_CLOEXEC);
    if (dfd < 0) { printf("  %-6s open(%s) failed\n", label, node); return -1; }
    struct gbm_device *gbm = gbm_create_device(dfd);
    if (!gbm) { printf("  %-6s gbm_create_device failed\n", label); close(dfd); return -1; }
    printf("  %-6s gbm backend = %s\n", label, gbm_device_get_backend_name(gbm));

    uint64_t linear = DRM_FORMAT_MOD_LINEAR;
    struct gbm_bo *bo = gbm_bo_create_with_modifiers(gbm, W, H,
                                                     GBM_FORMAT_XRGB8888,
                                                     &linear, 1);
    if (!bo) {
        printf("  %-6s gbm_bo_create_with_modifiers(LINEAR) FAILED\n", label);
        /* fall back: plain create, then see what modifier we got */
        bo = gbm_bo_create(gbm, W, H, GBM_FORMAT_XRGB8888,
                           GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
        if (!bo) { printf("  %-6s gbm_bo_create(USE_LINEAR) FAILED too\n", label); return -1; }
        printf("  %-6s ...but gbm_bo_create(USE_LINEAR) worked\n", label);
    }
    out->bo     = bo;
    out->mod    = gbm_bo_get_modifier(bo);
    out->stride = gbm_bo_get_stride(bo);
    out->fd     = gbm_bo_get_fd(bo);
    printf("  %-6s ALLOCATED modifier=0x%016llx stride=%u fd=%d\n",
           label, (unsigned long long)out->mod, out->stride, out->fd);
    if (out->mod != DRM_FORMAT_MOD_LINEAR)
        printf("  %-6s !! not LINEAR after all\n", label);
    return out->fd < 0 ? -1 : 0;
}

int main(void)
{
    printf("== stage 1/2: allocate a LINEAR XRGB8888 buffer ==\n");
    struct buf nv = {0}, amd = {0};
    int have_nv  = alloc_linear("NVIDIA", "/dev/dri/renderD128", &nv)  == 0;
    /* CONTROL: a driver-chosen (block-linear) bo on the same device.  If this
     * also fails to bind, the harness is wrong, not the driver. */
    struct buf ctl = {0}; int have_ctl = 0;
    {
        int d = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        struct gbm_device *g = d >= 0 ? gbm_create_device(d) : NULL;
        struct gbm_bo *b = g ? gbm_bo_create(g, W, H, GBM_FORMAT_XRGB8888,
                                             GBM_BO_USE_RENDERING) : NULL;
        if (b) {
            ctl.bo = b; ctl.mod = gbm_bo_get_modifier(b);
            ctl.stride = gbm_bo_get_stride(b); ctl.fd = gbm_bo_get_fd(b);
            have_ctl = ctl.fd >= 0;
            printf("  %-6s ALLOCATED modifier=0x%016llx stride=%u fd=%d  (driver-chosen)\n",
                   "CTRL", (unsigned long long)ctl.mod, ctl.stride, ctl.fd);
        } else printf("  CTRL   allocation failed\n");
    }
    int have_amd = alloc_linear("AMD",    "/dev/dri/renderD129", &amd) == 0;
    if (!have_nv && !have_amd) { printf("nothing allocated; stop\n"); return 1; }

    printf("\n== EGL on the NVIDIA node ==\n");
    pGetPlatformDisplay = (void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
    int nfd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    struct gbm_device *ngbm = gbm_create_device(nfd);
    EGLDisplay dpy = pGetPlatformDisplay
        ? pGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, ngbm, NULL)
        : eglGetDisplay((EGLNativeDisplayType)ngbm);
    EGLint maj, min;
    if (!eglInitialize(dpy, &maj, &min)) { printf("eglInitialize failed 0x%x\n", eglGetError()); return 1; }
    printf("  EGL %d.%d  vendor=%s\n", maj, min, eglQueryString(dpy, EGL_VENDOR));

    eglBindAPI(EGL_OPENGL_API);
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, NULL);
    if (ctx == EGL_NO_CONTEXT) {
        EGLint cfgattr[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
                             EGL_OPENGL_BIT, EGL_NONE };
        EGLConfig cfg; EGLint n;
        eglChooseConfig(dpy, cfgattr, &cfg, 1, &n);
        ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, NULL);
    }
    if (ctx == EGL_NO_CONTEXT) { printf("eglCreateContext failed 0x%x\n", eglGetError()); return 1; }
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
    printf("  GL_RENDERER = %s\n", glGetString(GL_RENDERER));

    pCreateImage             = (void*)eglGetProcAddress("eglCreateImageKHR");
    pDestroyImage            = (void*)eglGetProcAddress("eglDestroyImageKHR");
    pEGLImageTargetTexture2D = (void*)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    pCopyImageSubData        = (void*)eglGetProcAddress("glCopyImageSubData");
    pGenFramebuffers         = (void*)eglGetProcAddress("glGenFramebuffers");
    pBindFramebuffer         = (void*)eglGetProcAddress("glBindFramebuffer");
    pFramebufferTexture2D    = (void*)eglGetProcAddress("glFramebufferTexture2D");
    pCheckFramebufferStatus  = (void*)eglGetProcAddress("glCheckFramebufferStatus");
    if (!pCreateImage || !pEGLImageTargetTexture2D || !pCopyImageSubData) {
        printf("  missing entry points (createimage=%p target=%p copyimage=%p)\n",
               (void*)pCreateImage, (void*)pEGLImageTargetTexture2D, (void*)pCopyImageSubData);
        return 1;
    }

    /* a normal, driver-chosen (block-linear) source texture, filled */
    GLuint src; glGenTextures(1, &src);
    glBindTexture(GL_TEXTURE_2D, src);
    unsigned *px = malloc(W*H*4);
    for (int i = 0; i < W*H; i++) px[i] = 0xff00ff00u;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_BGRA, GL_UNSIGNED_BYTE, px);
    printf("  source texture: err=0x%x\n", glGetError());

    struct { const char *name; struct buf *b; int ok; } cases[] = {
        { "CONTROL: driver-chosen (block-linear)", &ctl, have_ctl },
        { "NVIDIA-allocated LINEAR", &nv,  have_nv  },
        { "AMD-allocated LINEAR",    &amd, have_amd },
    };

    for (unsigned c = 0; c < 3; c++) {
        if (!cases[c].ok) continue;
        printf("\n== %s ==\n", cases[c].name);
        EGLint attr[] = {
            EGL_WIDTH, W, EGL_HEIGHT, H,
            EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
            EGL_DMA_BUF_PLANE0_FD_EXT, cases[c].b->fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)cases[c].b->stride,
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(cases[c].b->mod & 0xffffffff),
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(cases[c].b->mod >> 32),
            EGL_NONE
        };
        /* IMPLICIT first: no modifier attributes at all.  A driver may accept
         * a buffer it refuses to be told is LINEAR. */
        EGLint attr_imp[] = {
            EGL_WIDTH, W, EGL_HEIGHT, H,
            EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
            EGL_DMA_BUF_PLANE0_FD_EXT, cases[c].b->fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)cases[c].b->stride,
            EGL_NONE
        };
        EGLImageKHR imp = pCreateImage(dpy, EGL_NO_CONTEXT,
                                       EGL_LINUX_DMA_BUF_EXT, NULL, attr_imp);
        if (imp == EGL_NO_IMAGE_KHR) {
            printf("  [implicit] eglCreateImageKHR FAILED err=0x%x\n", eglGetError());
        } else {
            GLuint t2; glGenTextures(1, &t2); glBindTexture(GL_TEXTURE_2D, t2);
            while (glGetError() != GL_NO_ERROR) {}
            pEGLImageTargetTexture2D(GL_TEXTURE_2D, imp);
            GLenum e2 = glGetError();
            printf("  [implicit, NO modifier attrs] bind: err=0x%x  %s\n",
                   e2, e2 ? "REFUSED" : "*** ACCEPTED ***");
            glDeleteTextures(1, &t2);
            pDestroyImage(dpy, imp);
        }

        EGLImageKHR img = pCreateImage(dpy, EGL_NO_CONTEXT,
                                       EGL_LINUX_DMA_BUF_EXT, NULL, attr);
        if (img == EGL_NO_IMAGE_KHR) {
            printf("  IMPORT FAILED: eglCreateImageKHR err=0x%x  <-- NVIDIA will not import it\n",
                   eglGetError());
            continue;
        }
        printf("  import OK\n");

        GLuint dst; glGenTextures(1, &dst);
        glBindTexture(GL_TEXTURE_2D, dst);
        pEGLImageTargetTexture2D(GL_TEXTURE_2D, img);
        GLenum e = glGetError();
        printf("  bind as texture: err=0x%x %s\n", e, e ? "(FAILED)" : "(ok)");
        if (e) { pDestroyImage(dpy, img); continue; }

        /* 4: FBO bind -- the KNOWN refusal, reproduced as a control */
        GLuint fbo; pGenFramebuffers(1, &fbo);
        pBindFramebuffer(GL_FRAMEBUFFER, fbo);
        pFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dst, 0);
        GLenum fe = glGetError();
        GLenum st = pCheckFramebufferStatus(GL_FRAMEBUFFER);
        printf("  [CONTROL] FBO render target: err=0x%x status=0x%x %s\n",
               fe, st, st == 0x8CD5 ? "COMPLETE" : "INCOMPLETE (expected)");
        pBindFramebuffer(GL_FRAMEBUFFER, 0);

        /* 5: THE QUESTION -- copy into it, never binding it as a render target */
        while (glGetError() != GL_NO_ERROR) {}
        pCopyImageSubData(src, GL_TEXTURE_2D, 0, 0,0,0,
                          dst, GL_TEXTURE_2D, 0, 0,0,0, W, H, 1);
        GLenum ce = glGetError();
        glFinish();
        printf("  [ANSWER]  glCopyImageSubData -> dst: err=0x%x  %s\n",
               ce, ce ? "REFUSED" : "*** ACCEPTED ***");
        pDestroyImage(dpy, img);
    }
    printf("\ndone\n");
    return 0;
}
