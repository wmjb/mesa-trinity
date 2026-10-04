/* shtest "<fragment shader body>" - renders a full-viewport tri, prints centre pixel */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
static const char *vs =
  "attribute vec4 pos;\nattribute vec4 col;\nvarying vec4 v;\n"
  "void main(){ gl_Position = pos; v = col; }\n";
static GLuint sh(GLenum t,const char *s){GLuint x=glCreateShader(t);glShaderSource(x,1,&s,NULL);glCompileShader(x);
  GLint ok=0;glGetShaderiv(x,GL_COMPILE_STATUS,&ok);
  if(!ok){char l[512];glGetShaderInfoLog(x,511,NULL,l);printf("COMPILE_FAIL %s\n",l);return 0;}return x;}
int main(int argc,char**argv){
  setbuf(stdout,NULL);
  /* Mesa built with the x11 platform makes EGL_DEFAULT_DISPLAY mean X11,
   * which is not there over ssh. Ask for surfaceless unless told otherwise. */
  setenv("EGL_PLATFORM", "surfaceless", 0);
  if(argc<2){printf("usage: shtest <fs body>\n");return 1;}
  char fs[4096];
  snprintf(fs,sizeof(fs),"precision mediump float;\nvarying vec4 v;\n%s\n",argv[1]);
  EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY);eglInitialize(d,0,0);eglBindAPI(EGL_OPENGL_ES_API);
  EGLint ca[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
               EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
  EGLConfig c;EGLint n;eglChooseConfig(d,ca,&c,1,&n);
  EGLint cx[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
  EGLContext ctx=eglCreateContext(d,c,EGL_NO_CONTEXT,cx);
  EGLint pb[]={EGL_WIDTH,64,EGL_HEIGHT,64,EGL_NONE};
  EGLSurface s=eglCreatePbufferSurface(d,c,pb);eglMakeCurrent(d,s,s,ctx);
  GLuint a=sh(GL_VERTEX_SHADER,vs),b=sh(GL_FRAGMENT_SHADER,fs);
  if(!a||!b)return 1;
  GLuint p=glCreateProgram();glAttachShader(p,a);glAttachShader(p,b);
  glBindAttribLocation(p,0,"pos");glBindAttribLocation(p,1,"col");glLinkProgram(p);
  GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok){printf("LINK_FAIL\n");return 1;}
  glUseProgram(p);
  static const GLfloat vtx[]={-1,-1, 3,-1, -1,3};
  static const GLfloat cols[]={1,0,0,1, 1,0,0,1, 1,0,0,1};
  glViewport(0,0,64,64);glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);
  glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,vtx);glEnableVertexAttribArray(0);
  glVertexAttribPointer(1,4,GL_FLOAT,GL_FALSE,0,cols);glEnableVertexAttribArray(1);
  glDrawArrays(GL_TRIANGLES,0,3);glFinish();
  unsigned char px[4];glReadPixels(32,32,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
  printf("%u,%u,%u,%u\n",px[0],px[1],px[2],px[3]);
  return 0;}
