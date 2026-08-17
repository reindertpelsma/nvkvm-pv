/* egl_dmabuf_export_probe.c — can NVIDIA EGL EXPORT a dma-buf inside nvkvm?
 *
 * WHY.  A Wayland GL client on NVIDIA gets a correct GPU context
 * (GL_RENDERER = "NVIDIA GeForce RTX 3060/PCIe/SSE2") and then contributes ZERO
 * pixels to the compositor: WAYLAND_DEBUG shows it bind zwp_linux_dmabuf_v1,
 * receive the full feedback (format_table / main_device / tranche_formats /
 * done) and then NEVER emit zwp_linux_buffer_params_v1.create or
 * wl_surface.attach.  Its main thread sits in pthread_cond_wait inside
 * libnvidia-egl-wayland.so.1 <- libEGL_nvidia.so.0 (i.e. inside eglSwapBuffers).
 *
 * So the client stalls while trying to produce the buffer, before any of it
 * reaches the wire.  The operation it must perform there is: render into a GPU
 * surface and EXPORT it as a dma-buf to hand to the compositor.  This probe
 * isolates exactly that primitive, with no compositor and no Wayland involved:
 *
 *     GL texture -> eglCreateImageKHR -> eglExportDMABUFImageQueryMESA
 *                                     -> eglExportDMABUFImageMESA (gets the fd)
 *
 * A pass means export works and the Wayland stall is higher up; a fail/hang
 * here localises the gap to dma-buf export out of the paravirtual GPU.
 *
 * build: gcc -O2 egl_dmabuf_export_probe.c -o egl_dmabuf_export_probe -lEGL -lGLESv2
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

static const char *estr(EGLint e){
    switch(e){
    case EGL_SUCCESS: return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
    case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
    case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
    case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
    case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
    case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
    default: { static char b[32]; snprintf(b,sizeof b,"0x%04x",e); return b; }
    }
}
#define CHK(msg) do{ EGLint _e=eglGetError(); \
    printf("    [%s] %s\n", estr(_e), msg); }while(0)

int main(void){
    setvbuf(stdout, NULL, _IONBF, 0);   /* so a HANG still shows progress */

    printf("== 1. open EGL display (surfaceless) ==\n");
    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay =
        (void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay dpy = EGL_NO_DISPLAY;
    if (getPlatformDisplay)
        dpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY) dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY){ printf("RESULT: FAIL no display\n"); return 1; }

    EGLint maj, min;
    if (!eglInitialize(dpy,&maj,&min)){ printf("RESULT: FAIL eglInitialize %s\n", estr(eglGetError())); return 1; }
    printf("    EGL %d.%d  vendor=%s\n", maj, min, eglQueryString(dpy, EGL_VENDOR));

    const char *exts = eglQueryString(dpy, EGL_EXTENSIONS);
    int has_export = exts && strstr(exts, "EGL_MESA_image_dma_buf_export") != NULL;
    printf("    EGL_MESA_image_dma_buf_export advertised: %s\n", has_export?"yes":"NO");

    printf("== 2. create context ==\n");
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cfga[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                      EGL_RED_SIZE,8, EGL_GREEN_SIZE,8, EGL_BLUE_SIZE,8, EGL_ALPHA_SIZE,8,
                      EGL_NONE };
    EGLConfig cfg; EGLint n=0;
    if (!eglChooseConfig(dpy,cfga,&cfg,1,&n) || n<1){ printf("RESULT: FAIL no config\n"); return 1; }
    EGLint ctxa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy,cfg,EGL_NO_CONTEXT,ctxa);
    if (ctx==EGL_NO_CONTEXT){ printf("RESULT: FAIL ctx %s\n", estr(eglGetError())); return 1; }
    if (!eglMakeCurrent(dpy,EGL_NO_SURFACE,EGL_NO_SURFACE,ctx)){
        printf("RESULT: FAIL makecurrent %s\n", estr(eglGetError())); return 1; }
    printf("    GL_RENDERER: %s\n", (const char*)glGetString(GL_RENDERER));

    printf("== 3. GPU texture 256x256 ==\n");
    GLuint tex=0; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D,tex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,256,256,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glFinish();
    printf("    glGetError=0x%04x\n", glGetError());

    printf("== 4. eglCreateImageKHR from the texture ==\n");
    PFNEGLCREATEIMAGEKHRPROC createImage = (void*)eglGetProcAddress("eglCreateImageKHR");
    if (!createImage){ printf("RESULT: FAIL no eglCreateImageKHR\n"); return 1; }
    EGLImageKHR img = createImage(dpy, ctx, EGL_GL_TEXTURE_2D_KHR,
                                  (EGLClientBuffer)(uintptr_t)tex, NULL);
    if (img==EGL_NO_IMAGE_KHR){ printf("RESULT: FAIL eglCreateImageKHR %s\n", estr(eglGetError())); return 1; }
    printf("    image created OK\n");

    if (!has_export){ printf("RESULT: FAIL export extension absent\n"); return 1; }

    printf("== 5. eglExportDMABUFImageQueryMESA  <-- the primitive under test ==\n");
    PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC q = (void*)eglGetProcAddress("eglExportDMABUFImageQueryMESA");
    PFNEGLEXPORTDMABUFIMAGEMESAPROC     x = (void*)eglGetProcAddress("eglExportDMABUFImageMESA");
    if (!q || !x){ printf("RESULT: FAIL export entrypoints missing\n"); return 1; }

    int fourcc=0, nplanes=0; EGLuint64KHR mods[4]={0,0,0,0};
    if (!q(dpy, img, &fourcc, &nplanes, mods)){
        printf("RESULT: FAIL eglExportDMABUFImageQueryMESA %s\n", estr(eglGetError())); return 1; }
    printf("    fourcc=0x%08x ('%c%c%c%c') planes=%d modifier=0x%llx\n",
           fourcc, fourcc&0xff,(fourcc>>8)&0xff,(fourcc>>16)&0xff,(fourcc>>24)&0xff,
           nplanes, (unsigned long long)mods[0]);

    printf("== 6. eglExportDMABUFImageMESA (actually get the fd) ==\n");
    int fds[4]={-1,-1,-1,-1}; EGLint strides[4]={0}, offsets[4]={0};
    if (!x(dpy, img, fds, strides, offsets)){
        printf("RESULT: FAIL eglExportDMABUFImageMESA %s\n", estr(eglGetError())); return 1; }
    printf("    fd=%d stride=%d offset=%d\n", fds[0], strides[0], offsets[0]);
    if (fds[0] < 0){ printf("RESULT: FAIL export returned no fd\n"); return 1; }

    printf("RESULT: PASS dma-buf export works (fd=%d)\n", fds[0]);
    close(fds[0]);
    return 0;
}
