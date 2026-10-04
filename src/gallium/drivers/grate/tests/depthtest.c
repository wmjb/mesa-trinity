/* Does depth actually occlude? Draw a near quad over the left half, then a far
 * quad over everything. With working depth the left half keeps the near colour. */
#include <stdio.h>
#include <stdlib.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
static const char *vs="attribute vec3 pos;void main(){gl_Position=vec4(pos,1.0);}";
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
  GLuint tex,fbo,rb; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D,tex);
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,W,H,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
  glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
  glGenRenderbuffers(1,&rb); glBindRenderbuffer(GL_RENDERBUFFER,rb);
  glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT16,W,H);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,rb);
  GLuint p=glCreateProgram(); glAttachShader(p,sh(GL_VERTEX_SHADER,vs));
  glAttachShader(p,sh(GL_FRAGMENT_SHADER,fs)); glBindAttribLocation(p,0,"pos");
  glLinkProgram(p); glUseProgram(p); glViewport(0,0,W,H);
  GLint uc=glGetUniformLocation(p,"c");
  if (!getenv("NO_DEPTH")) {
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS); glDepthMask(GL_TRUE);
  }
  glClearColor(0,0,0,1); glClearDepthf(getenv("CLEARZ")?atof(getenv("CLEARZ")):1.0f);
  if (getenv("SPLIT_CLEAR")) {
    glClear(GL_DEPTH_BUFFER_BIT);
    glClear(GL_COLOR_BUFFER_BIT);
  } else
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
  /* near quad, left half, red */
  float zn = getenv("ZN")?atof(getenv("ZN")):-0.5f, zf = getenv("ZF")?atof(getenv("ZF")):0.5f;
  const GLfloat a[]={-1,-1,zn, 0,-1,zn, -1,1,zn, 0,1,zn};
  glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,0,a); glEnableVertexAttribArray(0);
  glUniform4f(uc,1,0,0,1); glDrawArrays(GL_TRIANGLE_STRIP,0,4);
  /* far quad, everything, green */
  const GLfloat b[]={-1,-1,zf, 1,-1,zf, -1,1,zf, 1,1,zf};
  glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,0,b);
  glUniform4f(uc,0,1,0,1); glDrawArrays(GL_TRIANGLE_STRIP,0,4);
  unsigned char px[4*4];
  glReadPixels(W/4,H/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
  glReadPixels(3*W/4,H/2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px+4);
  printf("zn=%.2f zf=%.2f ", zn, zf);
  printf("left(near,should stay RED)=%d,%d,%d  right(far,should be GREEN)=%d,%d,%d  %s\n",
     px[0],px[1],px[2],px[4],px[5],px[6],
     (px[0]>200&&px[1]<60&&px[4]<60&&px[5]>200)?"OCCLUSION OK":"OCCLUSION WRONG");
  return 0;
}
