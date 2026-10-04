/*
 * Put an unmistakable pattern on the real panel via KMS, so orientation and
 * corruption can be judged by eye instead of inferred from a readback whose
 * own conventions are what is in question.
 *
 *   top-left RED      top-right GREEN
 *   bottom-left BLUE  bottom-right WHITE
 *
 * plus a solid black bar along the very top edge.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

static const char *vs =
 "attribute vec2 pos;varying vec2 uv;"
 "void main(){uv=pos*0.5+0.5;gl_Position=vec4(pos,0.0,1.0);}";
/*
 * uv.y is 0 at NDC -1, which GL puts at the BOTTOM of the viewport. Two
 * step() calls give four flat quadrant colours with no mixing, so this needs
 * nothing beyond SGE - no LRP, no SEQ.
 *
 *   GL top-left  GREEN     GL top-right  YELLOW
 *   GL bot-left  BLACK     GL bot-right  RED
 */
static const char *fs =
 "precision mediump float;varying vec2 uv;"
 "void main(){gl_FragColor=vec4(step(0.5,uv.x),step(0.5,uv.y),0.0,1.0);}";
static GLuint sh(GLenum t,const char*s){GLuint x=glCreateShader(t);glShaderSource(x,1,&s,NULL);
 glCompileShader(x);GLint ok=0;glGetShaderiv(x,GL_COMPILE_STATUS,&ok);
 if(!ok){char l[512];glGetShaderInfoLog(x,511,NULL,l);printf("compile: %s\n",l);}return x;}

int main(void){
  setbuf(stdout,NULL);
  int fd=open("/dev/dri/card1",O_RDWR);
  if(fd<0){perror("open card1");return 1;}
  drmSetMaster(fd);
  drmModeRes *res=drmModeGetResources(fd);
  if(!res){perror("drmModeGetResources");return 1;}
  drmModeConnector *conn=NULL;
  for(int i=0;i<res->count_connectors;i++){
    drmModeConnector *c=drmModeGetConnector(fd,res->connectors[i]);
    if(c&&c->connection==DRM_MODE_CONNECTED&&c->count_modes>0){conn=c;break;}
    if(c)drmModeFreeConnector(c);
  }
  if(!conn){printf("no connected connector\n");return 1;}
  drmModeModeInfo mode=conn->modes[0];
  printf("panel: %s %ux%u\n",conn->modes[0].name,mode.hdisplay,mode.vdisplay);
  drmModeEncoder *enc=drmModeGetEncoder(fd,conn->encoder_id);
  uint32_t crtc_id=enc?enc->crtc_id:res->crtcs[0];

  struct gbm_device *gbm=gbm_create_device(fd);
  struct gbm_surface *surf=gbm_surface_create(gbm,mode.hdisplay,mode.vdisplay,
      GBM_FORMAT_XRGB8888,GBM_BO_USE_SCANOUT|GBM_BO_USE_RENDERING);
  if(!surf){printf("gbm_surface_create failed\n");return 1;}
  EGLDisplay d=eglGetDisplay((EGLNativeDisplayType)gbm);
  eglInitialize(d,0,0); eglBindAPI(EGL_OPENGL_ES_API);
  EGLint ca[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
               EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
  EGLConfig c;EGLint n; eglChooseConfig(d,ca,&c,1,&n);
  EGLint cx[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
  EGLContext ctx=eglCreateContext(d,c,EGL_NO_CONTEXT,cx);
  EGLSurface es=eglCreateWindowSurface(d,c,(EGLNativeWindowType)surf,NULL);
  eglMakeCurrent(d,es,es,ctx);
  printf("renderer: %s\n",glGetString(GL_RENDERER));

  GLuint p=glCreateProgram();
  glAttachShader(p,sh(GL_VERTEX_SHADER,vs));
  glAttachShader(p,sh(GL_FRAGMENT_SHADER,fs));
  glBindAttribLocation(p,0,"pos"); glLinkProgram(p); glUseProgram(p);
  static const float q[]={-1,-1, 1,-1, -1,1, 1,1};
  glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,q); glEnableVertexAttribArray(0);
  glViewport(0,0,mode.hdisplay,mode.vdisplay);
  glDrawArrays(GL_TRIANGLE_STRIP,0,4);
  eglSwapBuffers(d,es);

  struct gbm_bo *bo=gbm_surface_lock_front_buffer(surf);
  uint32_t handle=gbm_bo_get_handle(bo).u32, stride=gbm_bo_get_stride(bo), fb;
  printf("scanout bo: stride=%u\n",stride);
  if(drmModeAddFB(fd,mode.hdisplay,mode.vdisplay,24,32,stride,handle,&fb)){
    printf("drmModeAddFB failed: %s\n",strerror(errno)); return 1;}
  if(drmModeSetCrtc(fd,crtc_id,fb,0,0,&conn->connector_id,1,&mode)){
    printf("drmModeSetCrtc failed: %s\n",strerror(errno)); return 1;}
  printf("displayed. A correct screen reads:\n"
         "  top-left GREEN   top-right YELLOW\n"
         "  bot-left BLACK   bot-right RED\n");
  sleep(900);
  return 0;
}
