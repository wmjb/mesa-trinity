/* Render a vertical gradient into the default (winsys) framebuffer and report
 * which way up it landed. */
#include <stdio.h>
#include <stdlib.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
static const char *vs="attribute vec2 pos;varying vec2 p;void main(){p=pos*0.5+0.5;gl_Position=vec4(pos,0.0,1.0);}";
static const char *fs="precision mediump float;varying vec2 p;void main(){gl_FragColor=vec4(p.x,p.y,0.25,1.0);}";
static GLuint sh(GLenum t,const char*s){GLuint x=glCreateShader(t);glShaderSource(x,1,&s,NULL);glCompileShader(x);return x;}
int main(void){
  setbuf(stdout,NULL);
  /* Mesa built with the x11 platform makes EGL_DEFAULT_DISPLAY mean X11,
   * which is not there over ssh. Ask for surfaceless unless told otherwise. */
  setenv("EGL_PLATFORM", "surfaceless", 0);
  int W=256,H=256;
  EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY);eglInitialize(d,0,0);eglBindAPI(EGL_OPENGL_ES_API);
  EGLint ca[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
               EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
  EGLConfig c;EGLint n;eglChooseConfig(d,ca,&c,1,&n);
  EGLint cx[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
  EGLContext ctx=eglCreateContext(d,c,EGL_NO_CONTEXT,cx);
  EGLint pb[]={EGL_WIDTH,W,EGL_HEIGHT,H,EGL_NONE};
  EGLSurface s=eglCreatePbufferSurface(d,c,pb);eglMakeCurrent(d,s,s,ctx);
  GLuint p=glCreateProgram();glAttachShader(p,sh(GL_VERTEX_SHADER,vs));glAttachShader(p,sh(GL_FRAGMENT_SHADER,fs));
  glBindAttribLocation(p,0,"pos");glLinkProgram(p);glUseProgram(p);
  static const GLfloat v[]={-1,-1, 1,-1, -1,1,  1,-1, 1,1, -1,1};
  glViewport(0,0,W,H);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
  glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,v);glEnableVertexAttribArray(0);
  glDrawArrays(GL_TRIANGLES,0,6);glFinish();
  unsigned char lo[4],hi[4];
  glReadPixels(W/2,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,lo);      /* glReadPixels y=0 is bottom */
  glReadPixels(W/2,H-5,1,1,GL_RGBA,GL_UNSIGNED_BYTE,hi);
  printf("winsys fb: readpixels y=4 g=%u, y=%d g=%u  -> %s\n", lo[1], H-5, hi[1],
         (lo[1] < hi[1]) ? "OK (green grows upward, as GL expects)" : "FLIPPED");
  return 0;}
