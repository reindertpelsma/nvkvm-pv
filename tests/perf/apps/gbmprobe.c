/* gbmprobe.c — minimal NVIDIA EGL-GBM init probe. Opens a DRM card, makes a
 * gbm_device, gets an EGL display on the GBM platform, eglInitialize, and
 * prints the GL renderer string. Tells us exactly where NVIDIA-on-GBM fails
 * and whether we land on the GPU or llvmpipe.
 *   usage: gbmprobe /dev/dri/card0
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <gbm.h>
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <xf86drm.h>

int main(int argc, char **argv) {
    const char *node = argc > 1 ? argv[1] : "/dev/dri/card0";
    int fd = open(node, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    drmVersionPtr v = drmGetVersion(fd);
    printf("DRM driver: %s\n", v ? v->name : "?");
    if (v) drmFreeVersion(v);

    struct gbm_device *gbm = gbm_create_device(fd);
    if (!gbm) { printf("FAIL gbm_create_device\n"); return 2; }
    printf("gbm backend name: %s\n", gbm_device_get_backend_name(gbm));

    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatDisp =
        (void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy;
    if (getPlatDisp) dpy = getPlatDisp(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    else dpy = eglGetDisplay((EGLNativeDisplayType)gbm);
    if (dpy == EGL_NO_DISPLAY) { printf("FAIL eglGetPlatformDisplay\n"); return 3; }

    EGLint major, minor;
    if (!eglInitialize(dpy, &major, &minor)) {
        printf("FAIL eglInitialize: 0x%x\n", eglGetError());
        return 4;
    }
    printf("EGL %d.%d vendor=%s\n", major, minor, eglQueryString(dpy, EGL_VENDOR));

    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cfgattr[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                         EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                         EGL_RED_SIZE,8, EGL_GREEN_SIZE,8, EGL_BLUE_SIZE,8, EGL_NONE };
    EGLConfig cfg; EGLint n;
    if (!eglChooseConfig(dpy, cfgattr, &cfg, 1, &n) || n < 1) {
        printf("FAIL eglChooseConfig n=%d\n", n); return 5;
    }
    EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattr);
    if (ctx == EGL_NO_CONTEXT) { printf("FAIL eglCreateContext 0x%x\n", eglGetError()); return 6; }
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
    printf("GL_RENDERER: %s\n", (const char*)glGetString(GL_RENDERER));
    printf("GL_VENDOR:   %s\n", (const char*)glGetString(GL_VENDOR));
    printf("RESULT %s\n", strstr((const char*)glGetString(GL_RENDERER) ?: "", "NVIDIA") ? "GPU-OK" : "SOFTWARE");
    return 0;
}
