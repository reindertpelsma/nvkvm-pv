/* signal_restart_export.c — does a signal break a GPU export? (regression)
 *
 * Guards the bug that made every X11 client invisible through nvkvm.
 *
 * nvkvm_send_sync() waits interruptibly for the host round-trip on the isolate
 * ioctl path.  It used to answer a signal by aborting the in-flight host ioctl
 * and returning -ERESTARTSYS, so the kernel restarted the syscall.  But the
 * request is already on the wire by then, so the operation may ALREADY HAVE
 * TAKEN EFFECT -- and the export calls on that path are not idempotent:
 *
 *     'F' nr 0xd4                       park an RM object on an export fd
 *     'd' nr 0x49  GEM_EXPORT_NVKMS_MEMORY   associate a bo with it
 *
 * Both are steps of eglExportDMABUFImageMESA.  Repeat either on the same
 * handle -- which is exactly what a restarted syscall does -- and the driver
 * correctly refuses with EINVAL, because the export already exists.  NVIDIA's
 * EGL surfaces that refusal as GL_OUT_OF_MEMORY, which is why it was mistaken
 * for an allocation failure for a long time.
 *
 * Xwayland exposed it only because it arms a periodic SIGALRM; weston and
 * native Wayland clients do not, which is why they were always fine.  So this
 * test does the one thing Xwayland does differently: it arms a fast periodic
 * SIGALRM (SA_RESTART, as Xwayland's is) and then exports a dma-buf in a loop.
 * Without the fix some exports fail; with it none do.
 *
 * Deliberately does NOT need a compositor, an X server or a display -- it uses
 * EGL_EXT_platform_device, the same headless path tests/validate.sh uses, so it
 * runs anywhere the GPU comes up.
 *
 *   cc -O2 -o sigexport signal_restart_export.c -ldl
 *   ./sigexport [iterations]        # default 200
 *
 * Exit: 0 all exports succeeded, 1 at least one failed (the bug), 77 SKIP.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/time.h>

typedef void *EGLDisplay, *EGLContext, *EGLImageKHR, *EGLConfig, *EGLDeviceEXT;
typedef unsigned EGLenum, EGLBoolean;
typedef intptr_t EGLAttrib;
typedef int EGLint;

#define EGL_TRUE 1
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_PLATFORM_DEVICE_EXT 0x313F
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_NONE 0x3038
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_GL_TEXTURE_2D 0x30B1

#define GL_TEXTURE_2D 0x0DE1
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401

static EGLDisplay (*eglGetPlatformDisplay)(EGLenum, void *, const EGLAttrib *);
static EGLBoolean (*eglInitialize)(EGLDisplay, EGLint *, EGLint *);
static EGLBoolean (*eglBindAPI)(EGLenum);
static EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
static EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
static EGLBoolean (*eglMakeCurrent)(EGLDisplay, void *, void *, EGLContext);
static EGLImageKHR (*eglCreateImageKHR)(EGLDisplay, EGLContext, EGLenum, void *, const EGLint *);
static EGLBoolean (*eglDestroyImageKHR)(EGLDisplay, EGLImageKHR);
static EGLBoolean (*eglExportDMABUFImageQueryMESA)(EGLDisplay, EGLImageKHR, int *, int *, uint64_t *);
static EGLBoolean (*eglExportDMABUFImageMESA)(EGLDisplay, EGLImageKHR, int *, EGLint *, EGLint *);
static EGLint (*eglGetError)(void);
static void *(*eglGetProcAddress)(const char *);
static EGLBoolean (*eglQueryDevicesEXT)(EGLint, EGLDeviceEXT *, EGLint *);

static void (*glGenTextures)(int, unsigned *);
static void (*glBindTexture)(unsigned, unsigned);
static void (*glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void (*glDeleteTextures)(int, const unsigned *);

static volatile sig_atomic_t alarms;
static void on_alarm(int sig) { (void)sig; alarms++; }

static int skip(const char *why)
{
    printf("SKIP  signal_restart_export  %s\n", why);
    return 77;
}

int main(int argc, char **argv)
{
    int iters = (argc > 1) ? atoi(argv[1]) : 200;
    void *egl = dlopen("libEGL.so.1", RTLD_NOW);
    void *gl  = dlopen("libGLESv2.so.2", RTLD_NOW);
    if (!egl || !gl)
        return skip("libEGL.so.1 / libGLESv2.so.2 not present");

#define SYM(h, n) do { *(void **)(&n) = dlsym(h, #n); } while (0)
    SYM(egl, eglInitialize); SYM(egl, eglBindAPI); SYM(egl, eglChooseConfig);
    SYM(egl, eglCreateContext); SYM(egl, eglMakeCurrent); SYM(egl, eglGetError);
    SYM(egl, eglGetProcAddress);
    SYM(gl, glGenTextures); SYM(gl, glBindTexture); SYM(gl, glTexImage2D);
    SYM(gl, glDeleteTextures);
    if (!eglGetProcAddress || !eglInitialize)
        return skip("libEGL is missing core entry points");

    *(void **)(&eglGetPlatformDisplay) = eglGetProcAddress("eglGetPlatformDisplayEXT");
    *(void **)(&eglQueryDevicesEXT)    = eglGetProcAddress("eglQueryDevicesEXT");
    *(void **)(&eglCreateImageKHR)     = eglGetProcAddress("eglCreateImageKHR");
    *(void **)(&eglDestroyImageKHR)    = eglGetProcAddress("eglDestroyImageKHR");
    *(void **)(&eglExportDMABUFImageQueryMESA) = eglGetProcAddress("eglExportDMABUFImageQueryMESA");
    *(void **)(&eglExportDMABUFImageMESA)      = eglGetProcAddress("eglExportDMABUFImageMESA");
    if (!eglQueryDevicesEXT || !eglGetPlatformDisplay)
        return skip("EGL_EXT_platform_device not available");
    if (!eglExportDMABUFImageMESA || !eglCreateImageKHR)
        return skip("EGL_MESA_image_dma_buf_export not available");

    EGLDeviceEXT devs[8];
    EGLint ndev = 0;
    if (!eglQueryDevicesEXT(8, devs, &ndev) || ndev <= 0)
        return skip("no EGL devices");

    EGLDisplay dpy = EGL_NO_DISPLAY;
    for (EGLint i = 0; i < ndev; i++) {
        dpy = eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devs[i], NULL);
        if (dpy != EGL_NO_DISPLAY && eglInitialize(dpy, NULL, NULL)) break;
        dpy = EGL_NO_DISPLAY;
    }
    if (dpy == EGL_NO_DISPLAY)
        return skip("could not initialise any EGL device display");

    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg; EGLint ncfg = 0;
    if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg < 1)
        return skip("no suitable EGLConfig");

    const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (ctx == EGL_NO_CONTEXT)
        return skip("eglCreateContext failed");
    if (!eglMakeCurrent(dpy, NULL, NULL, ctx))
        return skip("eglMakeCurrent(surfaceless) failed");

    /*
     * Arm the thing that distinguishes Xwayland from every client that worked:
     * a fast periodic SIGALRM with SA_RESTART, so the kernel restarts any
     * syscall the signal lands in.  10 ms is chosen to be much shorter than an
     * export round-trip, so essentially every iteration gets interrupted --
     * without this the bug reproduces only occasionally.
     */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it = { { 0, 10000 }, { 0, 10000 } };
    setitimer(ITIMER_REAL, &it, NULL);

    int failed = 0, ok = 0;
    for (int i = 0; i < iters; i++) {
        unsigned tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, NULL);

        EGLImageKHR img = eglCreateImageKHR(dpy, ctx, EGL_GL_TEXTURE_2D,
                                            (void *)(uintptr_t)tex, NULL);
        if (!img) { failed++; glDeleteTextures(1, &tex); continue; }

        int fourcc = 0, nplanes = 0; uint64_t mod = 0;
        int fd = -1; EGLint stride = 0, offset = 0;
        if (eglExportDMABUFImageQueryMESA &&
            !eglExportDMABUFImageQueryMESA(dpy, img, &fourcc, &nplanes, &mod)) {
            failed++;
        } else if (!eglExportDMABUFImageMESA(dpy, img, &fd, &stride, &offset)) {
            failed++;
        } else {
            ok++;
            if (fd >= 0) close(fd);
        }
        eglDestroyImageKHR(dpy, img);
        glDeleteTextures(1, &tex);
    }

    it.it_value.tv_usec = 0; it.it_interval.tv_usec = 0;
    setitimer(ITIMER_REAL, &it, NULL);

    printf("%-5s signal_restart_export  %d/%d dma-buf exports succeeded under "
           "%d SIGALRMs (%d failed)\n",
           failed ? "FAIL" : "PASS", ok, iters, (int)alarms, failed);
    if (failed)
        puts("      A signal restarted a non-idempotent export ioctl.  See the "
             "X11 entry in docs/internal/known-limitations.md.");
    return failed ? 1 : 0;
}
