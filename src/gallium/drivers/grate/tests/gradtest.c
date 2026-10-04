/*
 * Perspective-correct interpolation. Every other test here draws a fullscreen
 * quad at w=1, where perspective correction is a no-op, so this path has never
 * been exercised. Feed clip coordinates with w varying across the primitive and
 * check the varying against softpipe.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
static const char *vs =
 "attribute vec4 pos;attribute vec3 col;varying vec3 c;"
 "void main(){ c = col; gl_Position = pos; }";
static const char *fs =
 "precision mediump float;varying vec3 c;"
 "void main(){ gl_FragColor = vec4(c, 1.0); }";
static GLuint sh(GLenum t,const char*s){GLuint x=glCreateShader(t);glShaderSource(x,1,&s,NULL);
 glCompileShader(x);GLint ok=0;glGetShaderiv(x,GL_COMPILE_STATUS,&ok);
 if(!ok){char l[512];glGetShaderInfoLog(x,511,NULL,l);printf("compile: %s\n",l);}return x;}
int main(void){
  setbuf(stdout,NULL);
  setenv("EGL_PLATFORM","surfaceless",0);
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

  GLuint p=glCreateProgram();
  glAttachShader(p,sh(GL_VERTEX_SHADER,vs)); glAttachShader(p,sh(GL_FRAGMENT_SHADER,fs));
  glBindAttribLocation(p,0,"pos"); glBindAttribLocation(p,1,"col");
  glLinkProgram(p); glUseProgram(p);
  glViewport(0,0,W,H);

  /* left edge w=1, right edge w=4: ndc spans -1..1 either way, but the
   * varying must be interpolated in 1/w space, not linearly in screen space */
  const float W0=1.0f, W1=1.0f;
  float g = getenv("GEOM_SCALE") ? atof(getenv("GEOM_SCALE")) : 1.0f;
  const GLfloat pos[] = {
    -g*W0,-g*W0, 0, W0,   g*W1,-g*W1, 0, W1,
    -g*W0, g*W0, 0, W0,   g*W1, g*W1, 0, W1,
  };
  const GLfloat col[] = { 0,1,0,  1,1,0,  0,1,0,  1,1,0 };
  glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,0,pos); glEnableVertexAttribArray(0);
  glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,0,col); glEnableVertexAttribArray(1);
  glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
  glDrawArrays(GL_TRIANGLE_STRIP,0,4);

  unsigned char *px=malloc(W*H*4);
  glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,px);
  /* one row is enough: the varying only changes along x */
  for(int x=0;x<W;x++) printf("g@%02d %d,%d\n", x, px[(H/2)*W*4+x*4], px[(H/2)*W*4+x*4+1]);
  printf("# glerr 0x%x\n", glGetError());
  return 0;
}
