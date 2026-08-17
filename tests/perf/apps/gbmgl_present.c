/* gbmgl_present.c — WIP harness: render a recognizable pattern INTO an NVIDIA
 * scanout bo and flip it on the nvkvm virtual head, so the host present-path
 * capture (#107) shows known content.
 *
 * STATUS / BLOCKER: rendering into a *forwarded* scanout bo from the guest is
 * unresolved. Two approaches fail on the guest NVIDIA EGL:
 *   - gbm_surface + eglSwapBuffers + gbm_surface_lock_front_buffer → swap
 *     succeeds but lock_front_buffer returns NULL (NVIDIA gbm doesn't populate
 *     the front-buffer queue the way Mesa does here).
 *   - gbm_bo_create + eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT | NATIVE_PIXMAP)
 *     → EGL_BAD_PARAMETER: the guest's dma-buf for a forwarded bo is a hollow
 *     proxy (the real bo lives in the stub), and NVIDIA EGL rejects importing it.
 * NVIDIA only binds a scanout bo as a render target through its internal
 * gbm_surface machinery (what a real compositor drives) — getting that to yield
 * a flippable bo on our virtual head is the open compositor-integration problem
 * (same root as "weston composites but never flips"). The present-path plumbing
 * (#106 export, #107 QEMU import+capture) is proven independently with gbmflip.
 *   usage: gbmgl_present [/dev/dri/card0] [nframes]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

static int flips;
static void pageflip(int fd,unsigned s,unsigned se,unsigned u,void*d){(void)fd;(void)s;(void)se;(void)u;(void)d;flips++;}

int main(int argc, char **argv)
{
    const char *node = argc > 1 ? argv[1] : "/dev/dri/card0";
    int nframes = argc > 2 ? atoi(argv[2]) : 30;
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open"); return 1; }
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    drmModeRes *res = drmModeGetResources(fd);
    if (!res) { perror("getres"); return 2; }
    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) { conn = c; break; }
        if (c) drmModeFreeConnector(c);
    }
    if (!conn) { fprintf(stderr, "no connected connector\n"); return 3; }
    drmModeModeInfo mode = conn->modes[0];
    int W = mode.hdisplay, H = mode.vdisplay;
    drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[0]);
    uint32_t crtc_id = (enc && enc->crtc_id) ? enc->crtc_id : res->crtcs[0];
    printf("connector %u %dx%d crtc %u\n", conn->connector_id, W, H, crtc_id);

    struct gbm_device *gbm = gbm_create_device(fd);
    if (!gbm) { fprintf(stderr, "gbm_create_device failed\n"); return 4; }
    printf("gbm backend: %s\n", gbm_device_get_backend_name(gbm));

    /* ── EGL surfaceless context ─────────────────────────────────────────── */
    EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)gbm);
    EGLint maj, min;
    if (!eglInitialize(dpy, &maj, &min)) { fprintf(stderr, "eglInitialize failed\n"); return 6; }
    printf("EGL %d.%d vendor=%s\n", maj, min, eglQueryString(dpy, EGL_VENDOR));
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                          EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
    EGLConfig cfg; EGLint ncfg;
    eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg);
    EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, ncfg ? cfg : EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) { fprintf(stderr, "eglCreateContext 0x%x\n", eglGetError()); return 7; }
    if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        fprintf(stderr, "eglMakeCurrent(surfaceless) 0x%x\n", eglGetError()); return 7;
    }
    printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));

    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR =
        (void *)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR =
        (void *)eglGetProcAddress("eglDestroyImageKHR");
    PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC glEGLImageTargetRenderbufferStorageOES =
        (void *)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
    if (!eglCreateImageKHR || !glEGLImageTargetRenderbufferStorageOES) {
        fprintf(stderr, "missing EGL/GL dma-buf import entrypoints\n"); return 8;
    }

    /* ── Scanout bo + EGLImage + FBO ─────────────────────────────────────── */
    struct gbm_bo *bo = gbm_bo_create(gbm, W, H, GBM_FORMAT_XRGB8888,
                                      GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!bo) { fprintf(stderr, "gbm_bo_create failed\n"); return 5; }
    uint32_t stride = gbm_bo_get_stride(bo);
    uint64_t mod = gbm_bo_get_modifier(bo);
    uint32_t handle = gbm_bo_get_handle_for_plane(bo, 0).u32;
    if (!handle) handle = gbm_bo_get_handle(bo).u32;
    int dbuf = gbm_bo_get_fd(bo);
    printf("bo: handle=0x%x stride=%u mod=0x%llx dmabuf=%d\n",
           handle, stride, (unsigned long long)mod, dbuf);

    /* Import the gbm_bo as an EGLImage directly (NATIVE_PIXMAP), keeping it in
     * NVIDIA's allocator namespace — the guest's dma-buf for a forwarded bo is a
     * hollow proxy and EGL_LINUX_DMA_BUF import rejects it (BAD_PARAMETER). */
    (void)dbuf; (void)stride;
    EGLImageKHR img = eglCreateImageKHR(dpy, ctx, EGL_NATIVE_PIXMAP_KHR,
                                        (EGLClientBuffer)bo, NULL);
    if (img == EGL_NO_IMAGE_KHR) { fprintf(stderr, "eglCreateImageKHR(pixmap) 0x%x\n", eglGetError()); return 8; }

    GLuint rb, fbo;
    glGenRenderbuffers(1, &rb);
    glBindRenderbuffer(GL_RENDERBUFFER, rb);
    glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, img);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "FBO incomplete 0x%x\n", st); return 9; }

    uint32_t fb = 0;
    uint32_t hs[4]={handle,0,0,0}, ss[4]={stride,0,0,0}, os[4]={0,0,0,0};
    uint64_t ms[4]={mod,0,0,0};
    int r;
    if (mod != DRM_FORMAT_MOD_LINEAR && mod != DRM_FORMAT_MOD_INVALID)
        r = drmModeAddFB2WithModifiers(fd, W, H, GBM_FORMAT_XRGB8888, hs, ss, os, ms, &fb, DRM_MODE_FB_MODIFIERS);
    else
        r = drmModeAddFB2(fd, W, H, GBM_FORMAT_XRGB8888, hs, ss, os, &fb, 0);
    if (r) { fprintf(stderr, "AddFB2 failed: %s\n", strerror(errno)); return 6; }

    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    drmEventContext ev = { .version = 2, .page_flip_handler = pageflip };

    for (int f = 0; f < nframes; f++) {
        glViewport(0, 0, W, H);
        glEnable(GL_SCISSOR_TEST);
        glScissor(0,   H/2, W/2, H/2); glClearColor(1,0,0,1); glClear(GL_COLOR_BUFFER_BIT); /* TL red   */
        glScissor(W/2, H/2, W/2, H/2); glClearColor(0,1,0,1); glClear(GL_COLOR_BUFFER_BIT); /* TR green */
        glScissor(0,   0,   W/2, H/2); glClearColor(0,0,1,1); glClear(GL_COLOR_BUFFER_BIT); /* BL blue  */
        glScissor(W/2, 0,   W/2, H/2); glClearColor(1,1,1,1); glClear(GL_COLOR_BUFFER_BIT); /* BR white */
        int bx = (f * (W / (nframes ? nframes : 1))) % W;
        glScissor(bx, 0, 12, H); glClearColor(1,1,0,1); glClear(GL_COLOR_BUFFER_BIT);       /* yellow band */
        glDisable(GL_SCISSOR_TEST);
        glFinish();

        if (f == 0) {
            r = drmModeSetCrtc(fd, crtc_id, fb, 0, 0, &conn->connector_id, 1, &mode);
            if (r) { fprintf(stderr, "SetCrtc failed: %s\n", strerror(errno)); break; }
        } else {
            r = drmModePageFlip(fd, crtc_id, fb, DRM_MODE_PAGE_FLIP_EVENT, NULL);
            if (!r && poll(&pfd, 1, 1000) > 0) drmHandleEvent(fd, &ev);
        }
        usleep(33000);
    }
    printf("RESULT flips_completed=%d frames=%d (rendered into scanout bo)\n", flips, nframes);
    (void)eglDestroyImageKHR;
    return 0;
}
