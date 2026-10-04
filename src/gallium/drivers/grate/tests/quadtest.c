/* Textured quad with interleaved pos+uv attributes and two triangles,
 * which is how weston's gl-renderer submits surfaces. */
#include <stdio.h>
#include <stdlib.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
static const char *vs =
  "attribute vec2 pos;\nattribute vec2 texcoord;\nvarying vec2 uv;\n"
  "void main(){ gl_Position = vec4(pos, 0.0, 1.0); uv = texcoord; }\n";
static const char *fs =
  "precision mediump float;\nvarying vec2 uv;\nuniform sampler2D t;\n"
  "void main(){ gl_FragColor = texture2D(t, uv); }\n";
static GLuint sh(GLenum ty,const char*s){GLuint x=glCreateShader(ty);glShaderSource(x,1,&s,NULL);glCompileShader(x);
  GLint ok=0;glGetShaderiv(x,GL_COMPILE_STATUS,&ok);if(!ok){char l[512];glGetShaderInfoLog(x,511,NULL,l);printf("compile fail %s\n",l);}return x;}
int main(void){
  setbuf(stdout,NULL);
  /* Mesa built with the x11 platform makes EGL_DEFAULT_DISPLAY mean X11,
   * which is not there over ssh. Ask for surfaceless unless told otherwise. */
  setenv("EGL_PLATFORM", "surfaceless", 0);
  EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY);eglInitialize(d,0,0);eglBindAPI(EGL_OPENGL_ES_API);
  EGLint ca[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
               EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
  EGLConfig c;EGLint n;eglChooseConfig(d,ca,&c,1,&n);
  EGLint cx[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
  EGLContext ctx=eglCreateContext(d,c,EGL_NO_CONTEXT,cx);
  EGLint pb[]={EGL_WIDTH,64,EGL_HEIGHT,64,EGL_NONE};
  EGLSurface s=eglCreatePbufferSurface(d,c,pb);eglMakeCurrent(d,s,s,ctx);

  GLuint tex;glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
  static unsigned char px[64*64*4];
  for(int y=0;y<64;y++)for(int x=0;x<64;x++){unsigned char*p=px+(y*64+x)*4;p[0]=x*4;p[1]=y*4;p[2]=64;p[3]=255;}
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,64,64,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);

  GLuint p=glCreateProgram();glAttachShader(p,sh(GL_VERTEX_SHADER,vs));glAttachShader(p,sh(GL_FRAGMENT_SHADER,fs));
  glBindAttribLocation(p,0,"pos");glBindAttribLocation(p,1,"texcoord");glLinkProgram(p);
  GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);if(!ok){printf("LINK_FAIL\n");return 1;}
  glUseProgram(p);
  glUniform1i(glGetUniformLocation(p,"t"),0);

  /* interleaved: x, y, u, v - two triangles forming a full-viewport quad */
  static const GLfloat v[] = {
    -1,-1, 0,0,   1,-1, 1,0,   -1,1, 0,1,
     1,-1, 1,0,   1, 1, 1,1,   -1,1, 0,1,
  };
  glViewport(0,0,64,64);glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);
  glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,4*sizeof(GLfloat),v);
  glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,4*sizeof(GLfloat),v+2);
  glEnableVertexAttribArray(0);glEnableVertexAttribArray(1);
  glDrawArrays(GL_TRIANGLES,0,6);glFinish();

  static const int pts[9][2]={{8,8},{32,8},{56,8},{8,32},{32,32},{56,32},{8,56},{32,56},{56,56}};
  for(int k=0;k<9;k++){unsigned char q[4];glReadPixels(pts[k][0],pts[k][1],1,1,GL_RGBA,GL_UNSIGNED_BYTE,q);
    printf("p%d %u,%u,%u,%u\n",k,q[0],q[1],q[2],q[3]);}
  printf("err 0x%x\n",glGetError());
  return 0;}
