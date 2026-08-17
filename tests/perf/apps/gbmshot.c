/* gbmshot.c — render a recognizable scene on the virtual head's GBM device with
 * NVIDIA EGL/GLES, read it back with glReadPixels, and write a PPM. Proves the
 * full GPU render path on /dev/dri/card0 (the nvkvm virtual KMS head) and yields
 * an actual image artifact. Surfaceless (FBO) to avoid gbm_surface format
 * negotiation. Usage: gbmshot /dev/dri/card0 /tmp/head.ppm
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <gbm.h>
#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#ifndef GL_RGBA8
#define GL_RGBA8 GL_RGBA8_OES
#endif

#define W 1280
#define H 720

static const char *vs =
  "attribute vec2 p; attribute vec3 c; varying vec3 vc;"
  "void main(){ vc=c; gl_Position=vec4(p,0.0,1.0); }";
static const char *fs =
  "precision mediump float; varying vec3 vc;"
  "void main(){ gl_FragColor=vec4(vc,1.0); }";

static GLuint mkshader(GLenum t, const char *s){
  GLuint sh=glCreateShader(t); glShaderSource(sh,1,&s,0); glCompileShader(sh);
  GLint ok; glGetShaderiv(sh,GL_COMPILE_STATUS,&ok);
  if(!ok){ char log[512]; glGetShaderInfoLog(sh,512,0,log); fprintf(stderr,"shader: %s\n",log);} 
  return sh;
}

int main(int argc, char**argv){
  const char *node = argc>1?argv[1]:"/dev/dri/card0";
  const char *out  = argc>2?argv[2]:"/tmp/head.ppm";
  int fd=open(node,O_RDWR); if(fd<0){perror("open");return 1;}
  struct gbm_device *gbm=gbm_create_device(fd);
  if(!gbm){fprintf(stderr,"gbm_create_device failed\n");return 2;}
  printf("gbm backend: %s\n", gbm_device_get_backend_name(gbm));

  PFNEGLGETPLATFORMDISPLAYEXTPROC getDisp=(void*)eglGetProcAddress("eglGetPlatformDisplayEXT");
  EGLDisplay dpy=getDisp(EGL_PLATFORM_GBM_KHR,gbm,0);
  EGLint maj,min; if(!eglInitialize(dpy,&maj,&min)){fprintf(stderr,"eglInitialize 0x%x\n",eglGetError());return 3;}
  eglBindAPI(EGL_OPENGL_ES_API);
  EGLint ca[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
               EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
  EGLConfig cfg; EGLint n; eglChooseConfig(dpy,ca,&cfg,1,&n);
  EGLint cx[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
  EGLContext ctx=eglCreateContext(dpy,cfg,EGL_NO_CONTEXT,cx);
  if(ctx==EGL_NO_CONTEXT){fprintf(stderr,"ctx 0x%x\n",eglGetError());return 4;}
  eglMakeCurrent(dpy,EGL_NO_SURFACE,EGL_NO_SURFACE,ctx);
  printf("GL_RENDERER: %s\n",(char*)glGetString(GL_RENDERER));

  /* Offscreen FBO target. */
  GLuint rb,fb; glGenRenderbuffers(1,&rb); glBindRenderbuffer(GL_RENDERBUFFER,rb);
  glRenderbufferStorage(GL_RENDERBUFFER,GL_RGBA8,W,H);
  glGenFramebuffers(1,&fb); glBindFramebuffer(GL_FRAMEBUFFER,fb);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,rb);
  if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){fprintf(stderr,"fbo incomplete\n");return 5;}
  glViewport(0,0,W,H);

  GLuint prog=glCreateProgram();
  glAttachShader(prog,mkshader(GL_VERTEX_SHADER,vs));
  glAttachShader(prog,mkshader(GL_FRAGMENT_SHADER,fs));
  glBindAttribLocation(prog,0,"p"); glBindAttribLocation(prog,1,"c");
  glLinkProgram(prog); glUseProgram(prog);

  glClearColor(0.10f,0.23f,0.10f,1.0f);  /* nvkvm green */
  glClear(GL_COLOR_BUFFER_BIT);
  /* Three big colored triangles (R,G,B) — obviously GPU-rasterized geometry. */
  GLfloat verts[]={
    -0.9f,-0.7f, 1,0,0,   -0.1f,-0.7f, 1,0,0,   -0.5f, 0.7f, 1,0,0,
    -0.4f,-0.7f, 0,1,0,    0.4f,-0.7f, 0,1,0,    0.0f, 0.8f, 0,1,0,
     0.1f,-0.7f, 0,0.4f,1, 0.9f,-0.7f, 0,0.4f,1, 0.5f, 0.7f, 0,0.4f,1,
  };
  glVertexAttribPointer(0,2,GL_FLOAT,0,5*sizeof(GLfloat),verts);
  glVertexAttribPointer(1,3,GL_FLOAT,0,5*sizeof(GLfloat),verts+2);
  glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
  glDrawArrays(GL_TRIANGLES,0,9);
  glFinish();

  unsigned char *px=malloc(W*H*4);
  glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,px);
  FILE*f=fopen(out,"wb"); fprintf(f,"P6\n%d %d\n255\n",W,H);
  for(int y=H-1;y>=0;y--) for(int x=0;x<W;x++){ unsigned char*p=px+(y*W+x)*4; fwrite(p,1,3,f);} 
  fclose(f);
  printf("wrote %s (%dx%d)\n",out,W,H);
  return 0;
}
