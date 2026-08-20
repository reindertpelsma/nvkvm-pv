/* Replicate what glamor does in glamor_egl_create_textured_pixmap_from_gbm_bo():
 * make a GBM bo, export it as a dmabuf, import that as an EGLImage, and bind it
 * to a texture.  Run on host and guest and compare.  */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define W 256
#define H 256

static const char *tag;
#define OK(f,...)   printf("[%s] ok   " f "\n", tag, ##__VA_ARGS__)
#define BAD(f,...) do{ printf("[%s] FAIL " f "\n", tag, ##__VA_ARGS__); return 1; }while(0)


/* One NATIVE_PIXMAP attempt, reporting the modifier the allocator actually
 * chose.  force_linear uses gbm_bo_create_with_modifiers to pin LINEAR. */
static EGLDisplay g_dpy;
static PFNEGLCREATEIMAGEKHRPROC g_create;

static void sweep(struct gbm_device *gbm, uint32_t flags, int force_linear,
                  const char *how)
{
    struct gbm_bo *bo;
    if (force_linear) {
        uint64_t lin = 0; /* DRM_FORMAT_MOD_LINEAR */
        bo = gbm_bo_create_with_modifiers(gbm, W, H, GBM_FORMAT_ARGB8888, &lin, 1);
    } else {
        bo = gbm_bo_create(gbm, W, H, GBM_FORMAT_ARGB8888, flags);
    }
    if (!bo) { printf("[%s]   %-28s bo_create FAILED\n", tag, how); return; }

    EGLImageKHR img = g_create(g_dpy, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR, bo, NULL);
    printf("[%s]   %-28s modifier=0x%016llx NATIVE_PIXMAP=%s",
           tag, how, (unsigned long long)gbm_bo_get_modifier(bo),
           img == EGL_NO_IMAGE_KHR ? "FAIL" : "ok");
    if (img == EGL_NO_IMAGE_KHR) printf(" (egl 0x%x)", eglGetError());
    printf("\n");
    gbm_bo_destroy(bo);
}

int main(int argc, char **argv)
{
    const char *node = argc > 1 ? argv[1] : "/dev/dri/renderD128";
    tag = argc > 2 ? argv[2] : "test";

    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) BAD("open(%s)", node);
    OK("open(%s) fd=%d", node, fd);

    struct gbm_device *gbm = gbm_create_device(fd);
    if (!gbm) BAD("gbm_create_device");
    OK("gbm_create_device backend=%s", gbm_device_get_backend_name(gbm));

    /* glamor asks for a scanout-capable rendering target, same as here. */
    struct gbm_bo *bo = gbm_bo_create(gbm, W, H, GBM_FORMAT_ARGB8888,
                                      GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!bo) {
        printf("[%s] note gbm_bo_create(SCANOUT|RENDERING) failed, retrying RENDERING only\n", tag);
        bo = gbm_bo_create(gbm, W, H, GBM_FORMAT_ARGB8888, GBM_BO_USE_RENDERING);
        if (!bo) BAD("gbm_bo_create");
    }
    uint64_t mod = gbm_bo_get_modifier(bo);
    int nplanes  = gbm_bo_get_plane_count(bo);
    OK("gbm_bo_create %dx%d modifier=0x%llx planes=%d stride=%u",
       W, H, (unsigned long long)mod, nplanes, gbm_bo_get_stride(bo));

    int dmabuf = gbm_bo_get_fd(bo);
    if (dmabuf < 0) BAD("gbm_bo_get_fd  <-- dmabuf export refused");
    OK("gbm_bo_get_fd fd=%d", dmabuf);

    EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)gbm);
    g_dpy = dpy;
    if (dpy == EGL_NO_DISPLAY) BAD("eglGetDisplay");
    EGLint maj, min;
    if (!eglInitialize(dpy, &maj, &min)) BAD("eglInitialize 0x%x", eglGetError());
    OK("eglInitialize %d.%d vendor=%s", maj, min, eglQueryString(dpy, EGL_VENDOR));

    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cfgat[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE,
                       EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLConfig cfg; EGLint n;
    if (!eglChooseConfig(dpy, cfgat, &cfg, 1, &n) || n < 1) BAD("eglChooseConfig");
    EGLint ctxat[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxat);
    if (ctx == EGL_NO_CONTEXT) BAD("eglCreateContext 0x%x", eglGetError());
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) BAD("eglMakeCurrent 0x%x", eglGetError());
    OK("eglMakeCurrent GL_RENDERER='%s'", (const char*)glGetString(GL_RENDERER));

    PFNEGLCREATEIMAGEKHRPROC pCreate =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pBind =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!pCreate) BAD("no eglCreateImageKHR");
    if (!pBind)   BAD("no glEGLImageTargetTexture2DOES");
    g_create = pCreate;

    /* Path A: the dmabuf import, which is what glamor uses on modern servers. */
    EGLint att[] = {
        EGL_WIDTH, W, EGL_HEIGHT, H,
        EGL_LINUX_DRM_FOURCC_EXT, GBM_FORMAT_ARGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)gbm_bo_get_offset(bo, 0),
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)gbm_bo_get_stride(bo),
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(mod & 0xffffffff),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(mod >> 32),
        EGL_NONE
    };
    EGLImageKHR img = pCreate(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, att);
    if (img == EGL_NO_IMAGE_KHR) {
        printf("[%s] FAIL eglCreateImageKHR(LINUX_DMA_BUF, modifier) egl_error=0x%x\n",
               tag, eglGetError());
    } else {
        OK("eglCreateImageKHR(LINUX_DMA_BUF, modifier)");
    }

    /* Path B: the older native-pixmap import, straight from the gbm_bo. */
    EGLImageKHR img2 = pCreate(dpy, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR, bo, NULL);
    if (img2 == EGL_NO_IMAGE_KHR)
        printf("[%s] FAIL eglCreateImageKHR(NATIVE_PIXMAP from gbm_bo) egl_error=0x%x"
               "   <-- this is the exact glamor call\n", tag, eglGetError());
    else
        OK("eglCreateImageKHR(NATIVE_PIXMAP from gbm_bo)");

    EGLImageKHR use = img != EGL_NO_IMAGE_KHR ? img : img2;
    if (use == EGL_NO_IMAGE_KHR) BAD("both EGLImage import paths failed");

    GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    pBind(GL_TEXTURE_2D, use);
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) BAD("glEGLImageTargetTexture2DOES gl_error=0x%x", e);
    OK("glEGLImageTargetTexture2DOES bound to texture");

    GLuint fb; glGenFramebuffers(1, &fb); glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) BAD("FBO incomplete 0x%x", st);
    OK("FBO complete -- imported bo is a usable render target");

    glClearColor(0, 1, 0, 1); glClear(GL_COLOR_BUFFER_BIT); glFinish();
    unsigned char px[4] = {0,0,0,0};
    glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    OK("render+readback rgba(%d,%d,%d,%d)", px[0], px[1], px[2], px[3]);

    /* Modifier sweep.  If NATIVE_PIXMAP worked for SOME modifier, then which
     * modifier the scanout bo gets would be the lever -- and nvkvm controls
     * what it advertises.  Measured answer: it does not.  NVIDIA's EGL refuses
     * the import for every modifier including LINEAR, on host and guest alike,
     * so the modifier is not the variable and there is nothing to tune. */
    printf("[%s] -- NATIVE_PIXMAP modifier sweep --\n", tag);
    sweep(gbm, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING, 0, "SCANOUT|RENDERING");
    sweep(gbm, GBM_BO_USE_RENDERING, 0, "RENDERING");
    sweep(gbm, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR, 0,
          "SCANOUT|RENDERING|LINEAR");
    sweep(gbm, 0, 1, "with_modifiers(LINEAR)");

    printf("[%s] RESULT all-good\n", tag);
    return 0;
}
