/* Does NVIDIA's EGL CLAIM to support importing DRM_FORMAT_MOD_LINEAR? */
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <drm_fourcc.h>

int main(void)
{
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    struct gbm_device *g = gbm_create_device(fd);
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd =
        (void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay d = gpd(EGL_PLATFORM_GBM_KHR, g, NULL);
    EGLint a,b; eglInitialize(d,&a,&b);
    printf("EGL vendor: %s\n", eglQueryString(d, EGL_VENDOR));

    PFNEGLQUERYDMABUFMODIFIERSEXTPROC q =
        (void*)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    if (!q) { printf("no eglQueryDmaBufModifiersEXT\n"); return 1; }

    EGLuint64KHR mods[64]; EGLBoolean ext[64]; EGLint n = 0;
    if (!q(d, DRM_FORMAT_XRGB8888, 64, mods, ext, &n)) {
        printf("query failed 0x%x\n", eglGetError()); return 1;
    }
    printf("NVIDIA advertises %d modifiers for XRGB8888:\n", n);
    int has_linear = 0;
    for (int i = 0; i < n; i++) {
        if (mods[i] == DRM_FORMAT_MOD_LINEAR) has_linear = 1;
        printf("   0x%016llx%s\n", (unsigned long long)mods[i],
               mods[i] == DRM_FORMAT_MOD_LINEAR ? "   <-- LINEAR" : "");
    }
    printf("\nVERDICT: LINEAR is %sadvertised as importable by NVIDIA's own EGL\n",
           has_linear ? "" : "NOT ");
    return 0;
}
