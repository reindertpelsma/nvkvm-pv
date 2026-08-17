/*
 * fbo_formats_probe.c -- standalone offscreen-EGL FBO completeness probe.
 *
 * WHY THIS EXISTS.  validate.sh's gl_draw_pixel_check tests ONE attachment
 * configuration, so when it fails you cannot tell "this format is not
 * colour-renderable here" from "no format is".  That distinction is what
 * separated a driver quirk from an nvkvm bug in the 595/610
 * GL_FRAMEBUFFER_UNSUPPORTED investigation (tests/BOOT_MATRIX.md,
 * "Attribution"): every one of five formats failed identically with
 * glGetError() clean, which is not what an unsupported-format problem looks
 * like.
 *
 * Binds the NVIDIA EGL device via EGL_EXT_platform_device, so it needs no X
 * server and no Wayland compositor, and dlopen()s libEGL/libGLESv2 with
 * hand-rolled types so the only build dependency is a working cc.  Then, for
 * each of five colour attachment configurations, it creates an FBO and prints
 * glCheckFramebufferStatus() plus glGetError() after allocation, after
 * attachment, and after the status query.
 *
 * Runs unmodified on a bare-metal host and inside a guest -- that A/B is the
 * point.  Requires libegl1 + libgles2 present.
 *
 *   cc -O0 -o fbo_formats_probe fbo_formats_probe.c -ldl && ./fbo_formats_probe
 *
 * Healthy output ends with "SUMMARY| 0/5 configurations incomplete".
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>

typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext, *EGLDeviceEXT;
typedef unsigned int EGLenum, EGLBoolean;
typedef int EGLint;

#define EGL_NO_DISPLAY      ((EGLDisplay)0)
#define EGL_NO_CONTEXT      ((EGLContext)0)
#define EGL_NO_SURFACE      ((EGLSurface)0)
#define EGL_PLATFORM_DEVICE_EXT 0x313F
#define EGL_OPENGL_ES_API   0x30A0
#define EGL_NONE            0x3038
#define EGL_SURFACE_TYPE    0x3033
#define EGL_PBUFFER_BIT     0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT  0x0004
#define EGL_RED_SIZE        0x3024
#define EGL_GREEN_SIZE      0x3023
#define EGL_BLUE_SIZE       0x3022
#define EGL_ALPHA_SIZE      0x3021
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_WIDTH           0x3057
#define EGL_HEIGHT          0x3056

#define GL_RENDERER   0x1F01
#define GL_VENDOR     0x1F00
#define GL_VERSION    0x1F02
#define GL_FRAMEBUFFER      0x8D40
#define GL_RENDERBUFFER     0x8D41
#define GL_RGBA8            0x8058
#define GL_RGB565           0x8D62
#define GL_RGBA4            0x8056
#define GL_RGB5_A1          0x8057
#define GL_DEPTH_COMPONENT16 0x81A5
#define GL_TEXTURE_2D       0x0DE1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_NEAREST          0x2600
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT  0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_RGBA             0x1908
#define GL_UNSIGNED_BYTE    0x1401
#define GL_NO_ERROR         0

static const char *fbstat(unsigned s) {
    switch (s) {
        case 0x8CD5: return "GL_FRAMEBUFFER_COMPLETE";
        case 0x8CD6: return "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT";
        case 0x8CD7: return "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT";
        case 0x8CD9: return "GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS";
        case 0x8CDD: return "GL_FRAMEBUFFER_UNSUPPORTED";
        case 0x8D56: return "GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE";
        case 0:      return "ZERO (no bound framebuffer / error)";
        default:     return "?";
    }
}

int main(void) {
    void *E = dlopen("libEGL.so.1", RTLD_NOW);
    if (!E) E = dlopen("libEGL.so", RTLD_NOW);
    if (!E) { printf("dlopen libEGL: %s\n", dlerror()); return 2; }
    void *G = dlopen("libGLESv2.so.2", RTLD_NOW);
    if (!G) G = dlopen("libGLESv2.so", RTLD_NOW);
    if (!G) { printf("dlopen libGLESv2: %s\n", dlerror()); return 2; }

    void *(*eglGetProcAddress)(const char *) = dlsym(E, "eglGetProcAddress");
    EGLBoolean (*eglInitialize)(EGLDisplay, EGLint *, EGLint *) = dlsym(E, "eglInitialize");
    EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = dlsym(E, "eglChooseConfig");
    EGLBoolean (*eglBindAPI)(EGLenum) = dlsym(E, "eglBindAPI");
    EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = dlsym(E, "eglCreateContext");
    EGLSurface (*eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *) = dlsym(E, "eglCreatePbufferSurface");
    EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = dlsym(E, "eglMakeCurrent");
    EGLint (*eglGetError)(void) = dlsym(E, "eglGetError");
    EGLDisplay (*eglGetDisplay)(void *) = dlsym(E, "eglGetDisplay");

    EGLBoolean (*eglQueryDevicesEXT)(EGLint, EGLDeviceEXT *, EGLint *) =
        eglGetProcAddress ? eglGetProcAddress("eglQueryDevicesEXT") : NULL;
    EGLDisplay (*eglGetPlatformDisplayEXT)(EGLenum, void *, const EGLint *) =
        eglGetProcAddress ? eglGetProcAddress("eglGetPlatformDisplayEXT") : NULL;

    EGLDisplay dpy = EGL_NO_DISPLAY; char how[160] = "";
    if (eglQueryDevicesEXT && eglGetPlatformDisplayEXT) {
        EGLDeviceEXT devs[16]; EGLint nd = 0;
        if (eglQueryDevicesEXT(16, devs, &nd) && nd > 0) {
            for (EGLint i = 0; i < nd; i++) {
                EGLDisplay d = eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, devs[i], NULL);
                if (d != EGL_NO_DISPLAY) {
                    EGLint maj = 0, min = 0;
                    if (eglInitialize(d, &maj, &min)) {
                        dpy = d;
                        snprintf(how, sizeof how, "EGL_EXT_platform_device dev %d/%d, EGL %d.%d", i, nd, maj, min);
                        break;
                    }
                }
            }
        }
    }
    if (dpy == EGL_NO_DISPLAY && eglGetDisplay) {
        EGLDisplay d = eglGetDisplay(NULL); EGLint maj = 0, min = 0;
        if (d != EGL_NO_DISPLAY && eglInitialize(d, &maj, &min)) {
            dpy = d; snprintf(how, sizeof how, "eglGetDisplay(default), EGL %d.%d", maj, min);
        }
    }
    if (dpy == EGL_NO_DISPLAY) { printf("no EGL display (eglGetError=0x%X)\n", eglGetError ? eglGetError() : 0); return 2; }
    printf("egl_display: %s\n", how);

    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cfgattr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                         EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig cfg; EGLint ncfg = 0;
    if (!eglChooseConfig(dpy, cfgattr, &cfg, 1, &ncfg) || ncfg < 1) { printf("no config\n"); return 2; }
    EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattr);
    if (ctx == EGL_NO_CONTEXT) { printf("no context (0x%X)\n", eglGetError ? eglGetError() : 0); return 2; }
    EGLint pbattr[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface ? eglCreatePbufferSurface(dpy, cfg, pbattr) : EGL_NO_SURFACE;
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
        if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { printf("no makecurrent\n"); return 2; }
        printf("egl_context: surfaceless\n");
    } else printf("egl_context: pbuffer 64x64\n");

    const unsigned char *(*glGetString)(unsigned) = dlsym(G, "glGetString");
    printf("GL_RENDERER='%s'\nGL_VENDOR='%s'\nGL_VERSION='%s'\n",
           (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VENDOR),
           (const char *)glGetString(GL_VERSION));

    unsigned (*glGetError)(void) = dlsym(G, "glGetError");
    void (*glGenFramebuffers)(int, unsigned *) = dlsym(G, "glGenFramebuffers");
    void (*glBindFramebuffer)(unsigned, unsigned) = dlsym(G, "glBindFramebuffer");
    void (*glGenTextures)(int, unsigned *) = dlsym(G, "glGenTextures");
    void (*glBindTexture)(unsigned, unsigned) = dlsym(G, "glBindTexture");
    void (*glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *) = dlsym(G, "glTexImage2D");
    void (*glTexParameteri)(unsigned, unsigned, int) = dlsym(G, "glTexParameteri");
    void (*glFramebufferTexture2D)(unsigned, unsigned, unsigned, unsigned, int) = dlsym(G, "glFramebufferTexture2D");
    void (*glGenRenderbuffers)(int, unsigned *) = dlsym(G, "glGenRenderbuffers");
    void (*glBindRenderbuffer)(unsigned, unsigned) = dlsym(G, "glBindRenderbuffer");
    void (*glRenderbufferStorage)(unsigned, unsigned, int, int) = dlsym(G, "glRenderbufferStorage");
    void (*glFramebufferRenderbuffer)(unsigned, unsigned, unsigned, unsigned) = dlsym(G, "glFramebufferRenderbuffer");
    unsigned (*glCheckFramebufferStatus)(unsigned) = dlsym(G, "glCheckFramebufferStatus");
    if (!glGenFramebuffers || !glCheckFramebufferStatus) { printf("missing FBO entry points\n"); return 2; }

    int bad = 0;
    struct { const char *name; int is_tex; unsigned ifmt; } cases[] = {
        { "GL_RGBA/GL_UNSIGNED_BYTE texture", 1, GL_RGBA    },
        { "GL_RGBA8 renderbuffer",            0, GL_RGBA8   },
        { "GL_RGB565 renderbuffer",           0, GL_RGB565  },
        { "GL_RGBA4 renderbuffer",            0, GL_RGBA4   },
        { "GL_RGB5_A1 renderbuffer",          0, GL_RGB5_A1 },
    };
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        unsigned fbo = 0; glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        unsigned e_alloc, e_attach;
        if (cases[c].is_tex) {
            unsigned tex = 0; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, (int)cases[c].ifmt, 64, 64, 0, cases[c].ifmt, GL_UNSIGNED_BYTE, NULL);
            e_alloc = glGetError();
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
            e_attach = glGetError();
        } else {
            unsigned rb = 0; glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb);
            glRenderbufferStorage(GL_RENDERBUFFER, cases[c].ifmt, 64, 64);
            e_alloc = glGetError();
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
            e_attach = glGetError();
        }
        unsigned st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        unsigned e_after = glGetError();
        printf("FBO| %-34s status=0x%X %-45s glGetError alloc=0x%X attach=0x%X after=0x%X\n",
               cases[c].name, st, fbstat(st), e_alloc, e_attach, e_after);
        if (st != GL_FRAMEBUFFER_COMPLETE) bad++;
    }
    printf("SUMMARY| %d/%d configurations incomplete\n", bad, (int)(sizeof cases / sizeof cases[0]));
    fflush(stdout);
    _exit(bad ? 1 : 0);
}
