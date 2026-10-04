/* Sample a 256x256 texture into a small viewport: heavy minification, which
 * no other test here covers. */
#include <stdio.h>
#include <stdlib.h>
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
  int W=getenv("VP")?atoi(getenv("VP")):32, H=W;
  int TS=getenv("TS")?atoi(getenv("TS")):256;
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
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,W,H,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
  glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,rt,0);
  /* source texture: smooth gradient, no high frequencies, so minification
   * without mipmaps still has a well defined answer */
  unsigned char *tx=malloc(TS*TS*4);
  for(int y=0;y<TS;y++)for(int x=0;x<TS;x++){int o=(y*TS+x)*4;
    int hf = ((x/3)^(y/5)) & 1;
    tx[o]=hf?230:40; tx[o+1]=(x*7)&255; tx[o+2]=hf?60:200; tx[o+3]=255;}
  GLuint tex; glGenTextures(1,&tex); glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,tex);
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,TS,TS,0,GL_RGBA,GL_UNSIGNED_BYTE,tx);
  int lin = getenv("LINEAR") != NULL;
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,lin?GL_LINEAR:GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,lin?GL_LINEAR:GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
  GLuint p=glCreateProgram(); glAttachShader(p,sh(GL_VERTEX_SHADER,vs));
  glAttachShader(p,sh(GL_FRAGMENT_SHADER,fs)); glBindAttribLocation(p,0,"pos");
  glLinkProgram(p); glUseProgram(p); glUniform1i(glGetUniformLocation(p,"t"),0);
  glViewport(0,0,W,H);
  const GLfloat q[]={-1,-1, 1,-1, -1,1, 1,1};
  glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,q); glEnableVertexAttribArray(0);
  glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
  glDrawArrays(GL_TRIANGLE_STRIP,0,4);
  unsigned char *px=malloc(W*H*4);
  glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,px);
  for(int x=0;x<W;x+=1){int o=(H/2)*W*4+x*4; printf("m@%02d %d,%d,%d\n",x,px[o],px[o+1],px[o+2]);}
  return 0;
}
