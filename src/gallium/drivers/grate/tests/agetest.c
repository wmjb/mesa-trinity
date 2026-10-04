/* weston repaints only the damaged region when EGL_EXT_buffer_age tells it the
 * buffer it is about to draw into already holds frame N-age. A wrong age
 * leaves whatever the buffer had in it showing. */
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <stdint.h>
int main(void){
  setbuf(stdout,NULL);
  int fd=open("/dev/dri/card1",O_RDWR);
  struct gbm_device *g=gbm_create_device(fd);
  struct gbm_surface *s=gbm_surface_create(g,1366,768,GBM_FORMAT_XRGB8888,
      GBM_BO_USE_SCANOUT|GBM_BO_USE_RENDERING);
  EGLDisplay d=eglGetDisplay((EGLNativeDisplayType)g);
  eglInitialize(d,0,0); eglBindAPI(EGL_OPENGL_ES_API);
  const char *ext=eglQueryString(d,EGL_EXTENSIONS);
  printf("EGL_EXT_buffer_age      : %s\n", strstr(ext?ext:"","EGL_EXT_buffer_age")?"yes":"NO");
  printf("EGL_KHR_partial_update  : %s\n", strstr(ext?ext:"","EGL_KHR_partial_update")?"yes":"NO");
  EGLint ca[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
               EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_NONE};
  /* the config's native visual has to be the gbm surface's format or the
   * window surface comes out incomplete */
  EGLConfig cfgs[32]; EGLint n=0; eglChooseConfig(d,ca,cfgs,32,&n);
  EGLConfig c=NULL;
  for(int i=0;i<n;i++){ EGLint v=0;
    eglGetConfigAttrib(d,cfgs[i],EGL_NATIVE_VISUAL_ID,&v);
    if((uint32_t)v==GBM_FORMAT_XRGB8888){c=cfgs[i];break;} }
  if(!c){printf("no config matching XRGB8888 among %d\n",n); return 1;}
  EGLint cx[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
  EGLContext ctx=eglCreateContext(d,c,EGL_NO_CONTEXT,cx);
  EGLSurface es=eglCreateWindowSurface(d,c,(EGLNativeWindowType)s,NULL);
  eglMakeCurrent(d,es,es,ctx);
  /*
   * The contract: if eglQuerySurface reports age N, the buffer being drawn
   * into holds the frame from N swaps ago. weston leans on this to repaint
   * only the damaged region. Paint frame 0 solid red, then on later frames
   * paint only a small corner and read back a far away pixel: it has to still
   * be red, or every undamaged part of weston's screen is stale.
   */
  glViewport(0,0,1366,768);
  glClearColor(1.0f,0.0f,0.0f,1.0f); glClear(GL_COLOR_BUFFER_BIT);
  eglSwapBuffers(d,es);
  struct gbm_bo *b0=gbm_surface_lock_front_buffer(s);

  for(int i=1;i<5;i++){
    EGLint age=-1;
    eglQuerySurface(d,es,EGL_BUFFER_AGE_EXT,&age);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0,0,32,32);
    glClearColor(0.0f,1.0f,0.0f,1.0f); glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    unsigned char px[4]={0,0,0,0};
    glReadPixels(700,400,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);
    printf("frame %d: age=%d  far pixel = %3u,%3u,%3u  %s\n", i, age,
           px[0],px[1],px[2],
           (px[0]>200&&px[1]<60) ? "red, preserved" : "NOT preserved");
    eglSwapBuffers(d,es);
    struct gbm_bo *bo=gbm_surface_lock_front_buffer(s);
    if(b0){gbm_surface_release_buffer(s,b0);} b0=bo;
  }
  return 0;
}
