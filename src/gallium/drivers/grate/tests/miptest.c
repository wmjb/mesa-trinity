/* Explicit mip levels, each a flat distinct colour, sampled at a size that
 * should pick a known level. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
static const char *vs="attribute vec2 pos;varying vec2 uv;"
 "void main(){uv=pos*0.5+0.5;gl_Position=vec4(pos,0.0,1.0);}";
static const char *fs="precision mediump float;varying vec2 uv;uniform sampler2D t;"
 "void main(){gl_FragColor=texture2D(t,uv);}";
static GLuint sh(GLenum ty,const char*s){GLuint x=glCreateShader(ty);glShaderSource(x,1,&s,NULL);
 glCompileShader(x);return x;}
int main(void){
  setbuf(stdout,NULL); setenv("EGL_PLATFORM","surfaceless",0);
  int W=getenv("VP")?atoi(getenv("VP")):16;
  const int TS=256;
  EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY);eglInitialize(d,0,0);eglBindAPI(EGL_OPENGL_ES_API);
  EGLint ca[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
               EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
  EGLConfig c;EGLint n;eglChooseConfig(d,ca,&c,1,&n);
  EGLint cx[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
  EGLContext ctx=eglCreateContext(d,c,EGL_NO_CONTEXT,cx);
  EGLint pb[]={EGL_WIDTH,16,EGL_HEIGHT,16,EGL_NONE};
  EGLSurface s=eglCreatePbufferSurface(d,c,pb);eglMakeCurrent(d,s,s,ctx);
  printf("# renderer: %s\n", glGetString(GL_RENDERER));
  GLuint rt,fbo; glGenTextures(1,&rt); glBindTexture(GL_TEXTURE_2D,rt);
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,W,W,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
  glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,rt,0);
  GLuint tex; glGenTextures(1,&tex); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,tex);
  /* level L gets red = L*30, so the sampled colour names the level used */
  int lvl=0;
  for (int sz=TS; sz>=1; sz>>=1, lvl++) {
    unsigned char *b=malloc(sz*sz*4);
    for (int i=0;i<sz*sz;i++){b[i*4]=lvl*30; b[i*4+1]=64; b[i*4+2]=128; b[i*4+3]=255;}
    glTexImage2D(GL_TEXTURE_2D,lvl,GL_RGBA,sz,sz,0,GL_RGBA,GL_UNSIGNED_BYTE,b);
    free(b);
  }
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST_MIPMAP_NEAREST);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
  GLuint p=glCreateProgram(); glAttachShader(p,sh(GL_VERTEX_SHADER,vs));
  glAttachShader(p,sh(GL_FRAGMENT_SHADER,fs)); glBindAttribLocation(p,0,"pos");
  glLinkProgram(p); glUseProgram(p); glUniform1i(glGetUniformLocation(p,"t"),0);
  glViewport(0,0,W,W);
  const GLfloat q[]={-1,-1, 1,-1, -1,1, 1,1};
  glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,q); glEnableVertexAttribArray(0);
  glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
  glDrawArrays(GL_TRIANGLE_STRIP,0,4);
  unsigned char px[4];
  glReadPixels(W/2,W/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
  printf("vp=%d sampled level red=%d (level %d)\n", W, px[0], px[0]/30);
  return 0;
}
