/* Render a gradient into an FBO of a given width and read it back.
 * Catches render-target stride errors, which a power-of-two sized
 * pbuffer cannot. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdlib.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
static const char *vs="attribute vec2 pos;varying vec2 p;void main(){p=pos*0.5+0.5;gl_Position=vec4(pos,0.0,1.0);}";
static const char *fs="precision mediump float;varying vec2 p;void main(){gl_FragColor=vec4(p.x,p.y,0.25,1.0);}";
static GLuint sh(GLenum t,const char*s){GLuint x=glCreateShader(t);glShaderSource(x,1,&s,NULL);glCompileShader(x);
 GLint ok=0;glGetShaderiv(x,GL_COMPILE_STATUS,&ok);if(!ok){char l[512];glGetShaderInfoLog(x,511,NULL,l);printf("compile %s\n",l);}return x;}
int main(int argc,char**argv){
  setbuf(stdout,NULL);
  /* Mesa built with the x11 platform makes EGL_DEFAULT_DISPLAY mean X11,
   * which is not there over ssh. Ask for surfaceless unless told otherwise. */
  setenv("EGL_PLATFORM", "surfaceless", 0);
  int W = argc>1?atoi(argv[1]):300, H = argc>2?atoi(argv[2]):200;
  EGLDisplay d=eglGetDisplay(EGL_DEFAULT_DISPLAY);eglInitialize(d,0,0);eglBindAPI(EGL_OPENGL_ES_API);
  EGLint ca[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
               EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
  EGLConfig c;EGLint n;eglChooseConfig(d,ca,&c,1,&n);
  EGLint cx[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
  EGLContext ctx=eglCreateContext(d,c,EGL_NO_CONTEXT,cx);
  EGLint pb[]={EGL_WIDTH,16,EGL_HEIGHT,16,EGL_NONE};
  EGLSurface s=eglCreatePbufferSurface(d,c,pb);eglMakeCurrent(d,s,s,ctx);

  GLuint tex,fbo;
  glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,W,H,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
  glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tex,0);
  {GLenum st=glCheckFramebufferStatus(GL_FRAMEBUFFER); if(st!=GL_FRAMEBUFFER_COMPLETE){printf("FBO incomplete status=0x%04x glerr=0x%04x\n",st,glGetError());return 1;}}

  GLuint p=glCreateProgram();glAttachShader(p,sh(GL_VERTEX_SHADER,vs));glAttachShader(p,sh(GL_FRAGMENT_SHADER,fs));
  glBindAttribLocation(p,0,"pos");glLinkProgram(p);glUseProgram(p);
  /* a viewport-sized quad as two triangles, as weston submits */
  static const GLfloat v[]={-1,-1,  1,-1,  -1,1,   1,-1,  1,1,  -1,1};
  glViewport(0,0,W,H);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
  glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,v);glEnableVertexAttribArray(0);
  glDrawArrays(GL_TRIANGLES,0,6);glFinish();

  if (argc>3 && argv[3][0]=='h') {  /* map the rendered region per row */
    unsigned char q[4];
    for (int y=0; y<H; y += (H>=8?H/8:1)) {
      int last=-1;
      for (int x=0;x<W;x++){ glReadPixels(x,y,1,1,GL_RGBA,GL_UNSIGNED_BYTE,q);
        if (q[0]||q[1]) last=x; }
      printf("  row %4d: rendered up to x=%d of %d\n", y, last, W-1);
    }
    return 0;
  }
  if (argc>3) {   /* scan: first row whose value is wrong, read at mid width */
    unsigned char q[4];
    int x = W/2, bad = -1;
    int want_r = (int)((x + 0.5) / W * 255.0 + 0.5);
    for (int y=0;y<H;y++){
      glReadPixels(x,y,1,1,GL_RGBA,GL_UNSIGNED_BYTE,q);
      int want_g = (int)((y + 0.5) / H * 255.0 + 0.5);
      if (abs((int)q[0]-want_r) > 8 || abs((int)q[1]-want_g) > 8) { bad = y; break; }
    }
    if (bad < 0) printf("rt %dx%d: all %d rows correct\n", W, H, H);
    else printf("rt %dx%d: first wrong row = %d of %d\n", W, H, bad, H);
    return 0;
  }
  if (getenv("RT_SLEEP")) { glFinish(); usleep(atoi(getenv("RT_SLEEP")) * 1000); }
  if (getenv("RT_WARM")) { unsigned char w[4]; glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,w); }
  printf("rt %dx%d:", W, H);
  int pts[5][2]={{W/8,H/8},{W/2,H/8},{W/8,H/2},{W/2,H/2},{W-W/8,H-H/8}};
  for(int k=0;k<5;k++){unsigned char q[4];glReadPixels(pts[k][0],pts[k][1],1,1,GL_RGBA,GL_UNSIGNED_BYTE,q);
    printf(" (%d,%d)=%u,%u",pts[k][0],pts[k][1],q[0],q[1]);}
  printf("  err=0x%x\n",glGetError());
  return 0;}
