/* gl_decompose.c — decompose a GL frame into the primitives glmark2 stresses,
 * so a host-vs-guest score gap can be attributed instead of guessed.
 *
 * Headless: EGL_EXT_platform_device, no X / Wayland / compositor. Same path
 * glmark2 uses for its actual rendering, minus presentation.
 *
 * Sub-tests (each printed as METRIC <name> <rate>):
 *   drawcall_kcalls_s  many 1-triangle draws into a 16x16 FBO. Pure CPU-side
 *                      driver + submission rate; ~zero GPU work.
 *   fill_Gpix_s        full-viewport quads at 1920x1080. Pure GPU fill.
 *   mapwrite_GBs       glMapBufferRange(WRITE|INVALIDATE) + memcpy + unmap.
 *                      This is the H2 probe: if the guest's GPU-visible staging
 *                      memory is mapped UC/WC where the host's is WB, CPU
 *                      writes here collapse and nothing else does.
 *   bufsub_GBs         glBufferSubData of the same payload (driver's own copy).
 *   texsub_GBs         glTexSubImage2D upload.
 *   finish_us          glFinish round-trip after a trivial draw: the per-sync
 *                      control-path latency.
 *   readpix_GBs        glReadPixels 1920x1080 RGBA (device->host).
 *
 * Build: gcc gl_decompose.c -o gl_decompose -lEGL -lGLESv2 -lm
 */
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static const char *VS =
 "#version 300 es\nin vec2 p;out vec2 uv;\n"
 "void main(){uv=p*0.5+0.5;gl_Position=vec4(p,0.0,1.0);}\n";
static const char *FS =
 "#version 300 es\nprecision mediump float;in vec2 uv;out vec4 o;\n"
 "void main(){o=vec4(uv,0.5,1.0);}\n";
static GLuint sh(GLenum t,const char*s){
    GLuint o=glCreateShader(t);glShaderSource(o,1,&s,0);glCompileShader(o);
    GLint ok=0;glGetShaderiv(o,GL_COMPILE_STATUS,&ok);
    if(!ok){char l[2048];glGetShaderInfoLog(o,sizeof l,0,l);fprintf(stderr,"shader: %s\n",l);}
    return o;
}

int main(void){
    PFNEGLQUERYDEVICESEXTPROC qd=(void*)eglGetProcAddress("eglQueryDevicesEXT");
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if(!qd||!gpd){printf("CHECK gl_decompose FAIL no-platform-device\n");return 1;}
    EGLDeviceEXT devs[8];EGLint nd=0;
    if(!qd(8,devs,&nd)||nd<1){printf("CHECK gl_decompose FAIL no-egl-device\n");return 1;}
    EGLDisplay dpy=gpd(EGL_PLATFORM_DEVICE_EXT,devs[0],0);
    if(dpy==EGL_NO_DISPLAY||!eglInitialize(dpy,0,0)){printf("CHECK gl_decompose FAIL egl-init\n");return 1;}
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cfga[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_RED_SIZE,8,EGL_NONE};
    EGLConfig cfg;EGLint nc;
    if(!eglChooseConfig(dpy,cfga,&cfg,1,&nc)||nc<1){printf("CHECK gl_decompose FAIL no-config\n");return 1;}
    EGLint ctxa[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
    EGLContext ctx=eglCreateContext(dpy,cfg,EGL_NO_CONTEXT,ctxa);
    if(ctx==EGL_NO_CONTEXT){printf("CHECK gl_decompose FAIL no-context\n");return 1;}
    if(!eglMakeCurrent(dpy,EGL_NO_SURFACE,EGL_NO_SURFACE,ctx)){printf("CHECK gl_decompose FAIL makecurrent\n");return 1;}
    fprintf(stderr,"[gl] renderer=%s\n",(const char*)glGetString(GL_RENDERER));
    fprintf(stderr,"[gl] version=%s\n",(const char*)glGetString(GL_VERSION));

    GLuint prog=glCreateProgram();
    glAttachShader(prog,sh(GL_VERTEX_SHADER,VS));
    glAttachShader(prog,sh(GL_FRAGMENT_SHADER,FS));
    glBindAttribLocation(prog,0,"p");
    glLinkProgram(prog);glUseProgram(prog);

    /* one small FBO for draw-call rate, one big FBO for fill/readback */
    GLuint texS,fboS,texB,fboB;
    glGenTextures(1,&texS);glBindTexture(GL_TEXTURE_2D,texS);
    glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,16,16);
    glGenFramebuffers(1,&fboS);glBindFramebuffer(GL_FRAMEBUFFER,fboS);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texS,0);
    glGenTextures(1,&texB);glBindTexture(GL_TEXTURE_2D,texB);
    glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,1920,1080);
    glGenFramebuffers(1,&fboB);glBindFramebuffer(GL_FRAMEBUFFER,fboB);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texB,0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){printf("CHECK gl_decompose FAIL fbo\n");return 1;}

    static const float tri[]={-1,-1, 3,-1, -1,3};
    GLuint vbo;glGenBuffers(1,&vbo);glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof tri,tri,GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,0);

    /* ---- drawcall rate: tiny FBO, 1 triangle per draw ---- */
    glBindFramebuffer(GL_FRAMEBUFFER,fboS);glViewport(0,0,16,16);
    for(int i=0;i<1000;i++) glDrawArrays(GL_TRIANGLES,0,3);
    glFinish();
    {
        const int N=400000;double t0=now();
        for(int i=0;i<N;i++) glDrawArrays(GL_TRIANGLES,0,3);
        glFinish();double dt=now()-t0;
        printf("METRIC drawcall_kcalls_s %.1f\n",N/dt/1000.0);
    }

    /* ---- fill rate: full 1920x1080 viewport quads ---- */
    glBindFramebuffer(GL_FRAMEBUFFER,fboB);glViewport(0,0,1920,1080);
    for(int i=0;i<20;i++) glDrawArrays(GL_TRIANGLES,0,3);
    glFinish();
    {
        const int N=2000;double t0=now();
        for(int i=0;i<N;i++) glDrawArrays(GL_TRIANGLES,0,3);
        glFinish();double dt=now()-t0;
        printf("METRIC fill_Gpix_s %.3f\n",(double)N*1920.0*1080.0/dt/1e9);
    }

    /* ---- glFinish round-trip latency (control path per sync) ---- */
    glBindFramebuffer(GL_FRAMEBUFFER,fboS);glViewport(0,0,16,16);
    for(int i=0;i<200;i++){glDrawArrays(GL_TRIANGLES,0,3);glFinish();}
    {
        const int N=3000;double t0=now();
        for(int i=0;i<N;i++){glDrawArrays(GL_TRIANGLES,0,3);glFinish();}
        double dt=now()-t0;
        printf("METRIC finish_us %.2f\n",dt/N*1e6);
    }

    /* ---- CPU writes into GPU-visible memory: the cacheability probe ---- */
    const size_t SZ=8u<<20;                 /* 8 MB */
    unsigned char *src=malloc(SZ);memset(src,0xa5,SZ);
    GLuint sbo;glGenBuffers(1,&sbo);glBindBuffer(GL_ARRAY_BUFFER,sbo);
    glBufferData(GL_ARRAY_BUFFER,SZ,NULL,GL_STREAM_DRAW);
    {   /* map + memcpy + unmap */
        const int N=40;
        for(int i=0;i<4;i++){void*p=glMapBufferRange(GL_ARRAY_BUFFER,0,SZ,GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_BUFFER_BIT);
            if(p){memcpy(p,src,SZ);glUnmapBuffer(GL_ARRAY_BUFFER);} }
        glFinish();
        double t0=now();int ok=1;
        for(int i=0;i<N;i++){
            void*p=glMapBufferRange(GL_ARRAY_BUFFER,0,SZ,GL_MAP_WRITE_BIT|GL_MAP_INVALIDATE_BUFFER_BIT);
            if(!p){ok=0;break;}
            memcpy(p,src,SZ);
            glUnmapBuffer(GL_ARRAY_BUFFER);
        }
        glFinish();double dt=now()-t0;
        if(ok) printf("METRIC mapwrite_GBs %.3f\n",(double)N*SZ/dt/1e9);
        else   printf("METRIC mapwrite_GBs 0\n");
    }
    {   /* glBufferSubData: the driver does the copy */
        const int N=40;
        for(int i=0;i<4;i++) glBufferSubData(GL_ARRAY_BUFFER,0,SZ,src);
        glFinish();
        double t0=now();
        for(int i=0;i<N;i++) glBufferSubData(GL_ARRAY_BUFFER,0,SZ,src);
        glFinish();double dt=now()-t0;
        printf("METRIC bufsub_GBs %.3f\n",(double)N*SZ/dt/1e9);
    }
    {   /* texture upload */
        GLuint tu;glGenTextures(1,&tu);glBindTexture(GL_TEXTURE_2D,tu);
        glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,1024,1024);
        const size_t TSZ=1024u*1024u*4u;const int N=200;
        for(int i=0;i<10;i++) glTexSubImage2D(GL_TEXTURE_2D,0,0,0,1024,1024,GL_RGBA,GL_UNSIGNED_BYTE,src);
        glFinish();
        double t0=now();
        for(int i=0;i<N;i++) glTexSubImage2D(GL_TEXTURE_2D,0,0,0,1024,1024,GL_RGBA,GL_UNSIGNED_BYTE,src);
        glFinish();double dt=now()-t0;
        printf("METRIC texsub_GBs %.3f\n",(double)N*TSZ/dt/1e9);
    }
    {   /* readback */
        glBindFramebuffer(GL_FRAMEBUFFER,fboB);
        size_t RSZ=1920u*1080u*4u;unsigned char*dst=malloc(RSZ);
        const int N=60;
        glReadPixels(0,0,1920,1080,GL_RGBA,GL_UNSIGNED_BYTE,dst);
        double t0=now();
        for(int i=0;i<N;i++) glReadPixels(0,0,1920,1080,GL_RGBA,GL_UNSIGNED_BYTE,dst);
        double dt=now()-t0;
        printf("METRIC readpix_GBs %.3f\n",(double)N*RSZ/dt/1e9);
        free(dst);
    }
    printf("CHECK gl_decompose %s\n", glGetError()==GL_NO_ERROR?"ok":"FAIL");
    return 0;
}
