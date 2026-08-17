/* dmabuf_import_probe.c — allocate a gbm bo on card0, PRIME-export it, then
 * re-import the dma-buf via eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT) on the SAME
 * device.  This is the exact operation a compositor/capture does and that fails
 * on nvkvm ("importing the supplied dmabufs failed").  strace this to see
 * whether NVIDIA EGL's import does a PRIME_FD_TO_HANDLE handle round-trip
 * (→ forward to stub handle) or pins the dma-buf's pages (→ memfd page-swap).
 *   usage: dmabuf_import_probe [/dev/dri/card0]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

int main(int argc, char **argv)
{
    const char *node = argc > 1 ? argv[1] : "/dev/dri/card0";
    int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open"); return 1; }
    struct gbm_device *gbm = gbm_create_device(fd);
    if (!gbm) { fprintf(stderr, "gbm_create_device\n"); return 2; }
    fprintf(stderr, "gbm backend: %s\n", gbm_device_get_backend_name(gbm));

    EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)gbm);
    EGLint maj, min;
    if (!eglInitialize(dpy, &maj, &min)) { fprintf(stderr, "eglInitialize 0x%x\n", eglGetError()); return 3; }
    fprintf(stderr, "EGL %d.%d vendor=%s\n", maj, min, eglQueryString(dpy, EGL_VENDOR));
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ctxa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctxa);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);

    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR =
        (void *)eglGetProcAddress("eglCreateImageKHR");

    const int W = 256, H = 256;
    /* arg2: "linear" (default) or "blocklinear" — isolate whether NVIDIA EGL
     * import rejects the LINEAR modifier. */
    int want_linear = !(argc > 2 && !strcmp(argv[2], "blocklinear"));
    struct gbm_bo *bo;
    if (want_linear) {
        const uint64_t lin = DRM_FORMAT_MOD_LINEAR;
        bo = gbm_bo_create_with_modifiers(gbm, W, H, GBM_FORMAT_XRGB8888, &lin, 1);
        if (!bo) bo = gbm_bo_create(gbm, W, H, GBM_FORMAT_XRGB8888,
                                    GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
    } else {
        bo = gbm_bo_create(gbm, W, H, GBM_FORMAT_XRGB8888,
                           GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    }
    if (!bo) { fprintf(stderr, "gbm_bo_create\n"); return 4; }
    uint32_t stride = gbm_bo_get_stride(bo);
    uint64_t mod = gbm_bo_get_modifier(bo);
    int dbuf = gbm_bo_get_fd(bo);
    fprintf(stderr, "bo: stride=%u modifier=0x%llx dmabuf_fd=%d\n",
            stride, (unsigned long long)mod, dbuf);

    fprintf(stderr, ">>> eglCreateImageKHR(LINUX_DMA_BUF) BEGIN\n");
    EGLint attrs[] = {
        EGL_WIDTH, W, EGL_HEIGHT, H,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_XRGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, dbuf,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(mod & 0xffffffff),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(mod >> 32),
        EGL_NONE
    };
    EGLImageKHR img = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    fprintf(stderr, "<<< eglCreateImageKHR END: img=%p egl_err=0x%x\n", img, eglGetError());
    fprintf(stderr, "RESULT import=%s\n", img != EGL_NO_IMAGE_KHR ? "OK" : "FAILED");
    return img != EGL_NO_IMAGE_KHR ? 0 : 5;
}
