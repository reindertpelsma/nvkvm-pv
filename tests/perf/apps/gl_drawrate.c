/* gl_drawrate.c — isolate GL draw-call submission rate, nothing else.
 *
 * Headless (EGL_EXT_platform_device), 16x16 FBO, one 1-triangle glDrawArrays
 * per iteration, one glFinish at the end.  There is essentially no GPU work:
 * the number this prints is how fast the CPU-side driver can push commands.
 *
 * argv[1] = iterations (default 200000).  Prints ns per draw call.
 * Build: gcc -O2 gl_drawrate.c -o gl_drawrate -lEGL -lGLESv2
 */
#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9;}
static const char*VS="#version 300 es\nin vec2 p;void main(){gl_Position=vec4(p,0.,1.);}\n";
static const char*FS="#version 300 es\nprecision mediump float;out vec4 o;void main(){o=vec4(1.);}\n";
static GLuint sh(GLenum t,const char*s){GLuint o=glCreateShader(t);glShaderSource(o,1,&s,0);glCompileShader(o);return o;}
int main(int argc,char**argv){
    int N=argc>1?atoi(argv[1]):200000;
    PFNEGLQUERYDEVICESEXTPROC qd=(void*)eglGetProcAddress("eglQueryDevicesEXT");
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDeviceEXT d[8];EGLint nd=0;qd(8,d,&nd);
    EGLDisplay dpy=gpd(EGL_PLATFORM_DEVICE_EXT,d[0],0);eglInitialize(dpy,0,0);
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ca[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_RED_SIZE,8,EGL_NONE};
    EGLConfig cfg;EGLint nc;eglChooseConfig(dpy,ca,&cfg,1,&nc);
    EGLint xa[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
    EGLContext ctx=eglCreateContext(dpy,cfg,EGL_NO_CONTEXT,xa);
    eglMakeCurrent(dpy,EGL_NO_SURFACE,EGL_NO_SURFACE,ctx);
    GLuint p=glCreateProgram();glAttachShader(p,sh(GL_VERTEX_SHADER,VS));
    glAttachShader(p,sh(GL_FRAGMENT_SHADER,FS));glBindAttribLocation(p,0,"p");
    glLinkProgram(p);glUseProgram(p);
    GLuint tex,fbo;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
    glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,16,16);
    glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
    glViewport(0,0,16,16);
    static const float tri[]={-1,-1,3,-1,-1,3};
    GLuint vbo;glGenBuffers(1,&vbo);glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof tri,tri,GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,0);
    for(int i=0;i<2000;i++) glDrawArrays(GL_TRIANGLES,0,3);
    glFinish();
    double t0=now();
    for(int i=0;i<N;i++) glDrawArrays(GL_TRIANGLES,0,3);
    glFinish();
    double dt=now()-t0;
    printf("METRIC drawcall_ns %.1f\nMETRIC drawcall_kcalls_s %.1f\nMETRIC drawcall_wall_s %.3f\n",
           dt/N*1e9, N/dt/1000.0, dt);
    return 0;
}
