/* A draw longer than the 4096 vertices one packet can carry, so the splitting
 * path is exercised. Columns of triangles, each column a known colour. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
static const char *vs="attribute vec2 pos;attribute vec3 col;varying vec3 c;"
 "void main(){c=col;gl_Position=vec4(pos,0.0,1.0);}";
static const char *fs="precision mediump float;varying vec3 c;"
 "void main(){gl_FragColor=vec4(c,1.0);}";
static GLuint sh(GLenum t,const char*s){GLuint x=glCreateShader(t);glShaderSource(x,1,&s,NULL);
 glCompileShader(x);GLint ok=0;glGetShaderiv(x,GL_COMPILE_STATUS,&ok);
 if(!ok){char l[512];glGetShaderInfoLog(x,511,NULL,l);printf("compile %s\n",l);}return x;}
int main(int argc,char**argv){
  setbuf(stdout,NULL);
  setenv("EGL_PLATFORM","surfaceless",0);
  int W = getenv("FB_W")?atoi(getenv("FB_W")):64;
  int H = getenv("FB_H")?atoi(getenv("FB_H")):64;
  int ncol = argc>1?atoi(argv[1]):64;          /* columns of 2 triangles each */
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
  /* the apps that stripe all run with a depth buffer; mine did not */
  if (getenv("USE_DEPTH")) {
    GLuint rb; glGenRenderbuffers(1,&rb); glBindRenderbuffer(GL_RENDERBUFFER,rb);
    glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT16,W,H);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,rb);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(getenv("DEPTH_ALWAYS")?GL_ALWAYS:GL_LESS);
    if (getenv("NO_DEPTH_TEST")) glDisable(GL_DEPTH_TEST);
    if (getenv("DEPTH_MASK_OFF")) glDepthMask(GL_FALSE);
    printf("# depth test on, fbo status 0x%04x\n", glCheckFramebufferStatus(GL_FRAMEBUFFER));
  }
  GLuint p=glCreateProgram(); glAttachShader(p,sh(GL_VERTEX_SHADER,vs));
  glAttachShader(p,sh(GL_FRAGMENT_SHADER,fs));
  glBindAttribLocation(p,0,"pos"); glBindAttribLocation(p,1,"col");
  glLinkProgram(p); glUseProgram(p); glViewport(0,0,W,H);
  int nv = ncol*6;
  float *pos=malloc(nv*2*sizeof(float)), *col=malloc(nv*3*sizeof(float));
  for(int i=0;i<ncol;i++){
    float x0=-1.0f+2.0f*i/ncol, x1=-1.0f+2.0f*(i+1)/ncol;
    float v[6][2]={{x0,-1},{x1,-1},{x0,1},{x1,-1},{x1,1},{x0,1}};
    float r=(float)i/(ncol-1);
    for(int k=0;k<6;k++){
      pos[(i*6+k)*2+0]=v[k][0]; pos[(i*6+k)*2+1]=v[k][1];
      col[(i*6+k)*3+0]=r; col[(i*6+k)*3+1]=1.0f-r; col[(i*6+k)*3+2]=0.5f;
    }
  }
  glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,pos); glEnableVertexAttribArray(0);
  glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,0,col); glEnableVertexAttribArray(1);
  glClearColor(0,0,0,1); glClearDepthf(1.0f);
  glClear(GL_COLOR_BUFFER_BIT | (getenv("USE_DEPTH")?GL_DEPTH_BUFFER_BIT:0));
  glDrawArrays(GL_TRIANGLES,0,nv);
  unsigned char *px=malloc(W*H*4);
  glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,px);
  printf("# %d vertices in one draw\n", nv);
  for(int x=0;x<W;x+=1){int o=(H/2)*W*4+x*4; printf("b@%02d %d,%d,%d\n",x,px[o],px[o+1],px[o+2]);}
  return 0;
}
