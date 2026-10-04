/* Draw an opaque background, then a translucent quad over it. */
#include <stdio.h>
#include <stdlib.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
static const char *vs="attribute vec2 pos;void main(){gl_Position=vec4(pos,0.0,1.0);}";
static const char *fs="precision mediump float;uniform vec4 c;void main(){gl_FragColor=c;}";
static GLuint sh(GLenum t,const char*s){GLuint x=glCreateShader(t);glShaderSource(x,1,&s,NULL);
 glCompileShader(x);return x;}
int main(void){
  setbuf(stdout,NULL); setenv("EGL_PLATFORM","surfaceless",0);
  const int W=32,H=32;
  EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY);eglInitialize(d,0,0);eglBindAPI(EGL_OPENGL_ES_API);
  EGLint ca[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
               EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
  EGLConfig c;EGLint n;eglChooseConfig(d,ca,&c,1,&n);
  EGLint cx[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
  EGLContext ctx=eglCreateContext(d,c,EGL_NO_CONTEXT,cx);
  EGLint pb[]={EGL_WIDTH,16,EGL_HEIGHT,16,EGL_NONE};
  EGLSurface s=eglCreatePbufferSurface(d,c,pb);eglMakeCurrent(d,s,s,ctx);
  printf("# renderer: %s\n", glGetString(GL_RENDERER));
  GLuint tex,fbo; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D,tex);
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,W,H,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
  glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
  GLuint p=glCreateProgram(); glAttachShader(p,sh(GL_VERTEX_SHADER,vs));
  glAttachShader(p,sh(GL_FRAGMENT_SHADER,fs)); glBindAttribLocation(p,0,"pos");
  glLinkProgram(p); glUseProgram(p); glViewport(0,0,W,H);
  GLint uc=glGetUniformLocation(p,"c");
  const GLfloat q[]={-1,-1, 1,-1, -1,1, 1,1};
  glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,q); glEnableVertexAttribArray(0);
  glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
  /* opaque blue background */
  glDisable(GL_BLEND); glUniform4f(uc,0,0,1,1); glDrawArrays(GL_TRIANGLE_STRIP,0,4);
  /* red at 50% over it: expect (128,0,128) with SRC_ALPHA/ONE_MINUS_SRC_ALPHA */
  glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
  glUniform4f(uc,1,0,0,0.5f); glDrawArrays(GL_TRIANGLE_STRIP,0,4);
  unsigned char px[4];
  glReadPixels(W/2,H/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
  printf("blend result = %d,%d,%d (want about 128,0,128)\n", px[0],px[1],px[2]);
  return 0;
}
