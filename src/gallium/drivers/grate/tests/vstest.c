/* Vertex shader uniforms: transform a quad by a uniform mat4 and report where
 * the geometry actually landed. glxgears and glmark2 both put their
 * modelview-projection in uniforms, and both draw geometry at the wrong scale. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
static const char *vs =
 "attribute vec4 pos;uniform mat4 mvp;uniform vec4 tint;varying vec3 c;"
 "void main(){ c = tint.rgb; gl_Position = mvp * pos; }";
static const char *fs =
 "precision mediump float;varying vec3 c;"
 "void main(){ gl_FragColor = vec4(c,1.0); }";
static GLuint sh(GLenum t,const char*s){GLuint x=glCreateShader(t);glShaderSource(x,1,&s,NULL);
 glCompileShader(x);GLint ok=0;glGetShaderiv(x,GL_COMPILE_STATUS,&ok);
 if(!ok){char l[512];glGetShaderInfoLog(x,511,NULL,l);printf("compile: %s\n",l);}return x;}
int main(int argc,char**argv){
  setbuf(stdout,NULL);
  setenv("EGL_PLATFORM","surfaceless",0);
  const int W=64,H=64;
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
  glBindAttribLocation(p,0,"pos"); glLinkProgram(p); glUseProgram(p);
  glViewport(0,0,W,H);
  GLint umvp=glGetUniformLocation(p,"mvp"), utint=glGetUniformLocation(p,"tint");
  /* scale by 0.5 and shift right by 0.25: the quad must cover the middle half */
  float sc = argc>1?atof(argv[1]):0.5f, tx = argc>2?atof(argv[2]):0.25f;
  GLfloat m[16]={ sc,0,0,0,  0,sc,0,0,  0,0,1,0,  tx,0,0,1 };
  glUniformMatrix4fv(umvp,1,GL_FALSE,m);
  glUniform4f(utint,1.0f,0.0f,0.0f,1.0f);
  const GLfloat pos[]={-1,-1,0,1,  1,-1,0,1,  -1,1,0,1,  1,1,0,1};
  glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,0,pos); glEnableVertexAttribArray(0);
  glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
  glDrawArrays(GL_TRIANGLE_STRIP,0,4);
  unsigned char *px=malloc(W*H*4);
  glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,px);
  int first=-1,last=-1;
  for(int x=0;x<W;x++){ int r=px[(H/2)*W*4+x*4]; if(r>128){ if(first<0)first=x; last=x; } }
  printf("mvp scale=%.2f tx=%.2f -> red spans x %d..%d\n", sc, tx, first, last);
  printf("# glerr 0x%x\n", glGetError());
  return 0;
}
