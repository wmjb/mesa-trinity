/*
 * Weston does not render into an EGL window surface: it imports the scanout
 * buffer as a dmabuf, turns it into a renderbuffer and draws into that. Walk
 * the same path and check the bytes that land in the buffer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define W 1366
#define H 768

static GLuint sh(GLenum t, const char *s)
{
   GLuint x = glCreateShader(t);
   glShaderSource(x, 1, &s, NULL);
   glCompileShader(x);
   GLint ok; glGetShaderiv(x, GL_COMPILE_STATUS, &ok);
   if (!ok) { char log[512]; glGetShaderInfoLog(x, 512, NULL, log); printf("shader: %s\n", log); }
   return x;
}

int main(void)
{
   int fd = open("/dev/dri/renderD128", O_RDWR);
   struct gbm_device *gbm = gbm_create_device(fd);
   struct gbm_bo *bo = gbm_bo_create(gbm, W, H, GBM_FORMAT_XRGB8888,
                                     GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
   if (!bo) { printf("gbm_bo_create failed\n"); return 1; }

   int dmafd = gbm_bo_get_fd(bo);
   uint32_t stride = gbm_bo_get_stride(bo);
   printf("bo %dx%d stride=%u dmafd=%d\n", W, H, stride, dmafd);

   EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)gbm);
   eglInitialize(dpy, NULL, NULL);
   eglBindAPI(EGL_OPENGL_ES_API);
   EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext ctx = eglCreateContext(dpy, NULL, EGL_NO_CONTEXT, xa);
   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);

   EGLint ia[] = {
      EGL_WIDTH, W, EGL_HEIGHT, H,
      EGL_LINUX_DRM_FOURCC_EXT, GBM_FORMAT_XRGB8888,
      EGL_DMA_BUF_PLANE0_FD_EXT, dmafd,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
      EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
      EGL_NONE
   };
   PFNEGLCREATEIMAGEKHRPROC ci = (void *)eglGetProcAddress("eglCreateImageKHR");
   EGLImageKHR img = ci(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, ia);
   if (img == EGL_NO_IMAGE_KHR) { printf("import failed 0x%x\n", eglGetError()); return 1; }

   PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC rbs =
      (void *)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
   GLuint rb, fbo;
   glGenRenderbuffers(1, &rb);
   glBindRenderbuffer(GL_RENDERBUFFER, rb);
   rbs(GL_RENDERBUFFER, img);
   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
   printf("fbo status 0x%x glerr 0x%x\n", glCheckFramebufferStatus(GL_FRAMEBUFFER), glGetError());

   glViewport(0, 0, W, H);
   glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);

   /* a red band across the top third in GL coordinates, so the orientation of
    * what lands in memory is visible */
   static const char *vs = "attribute vec2 pos;void main(){gl_Position=vec4(pos,0.0,1.0);}";
   static const char *fs = "precision mediump float;void main(){gl_FragColor=vec4(1.0,0.0,0.0,1.0);}";
   GLuint p = glCreateProgram();
   glAttachShader(p, sh(GL_VERTEX_SHADER, vs));
   glAttachShader(p, sh(GL_FRAGMENT_SHADER, fs));
   glBindAttribLocation(p, 0, "pos");
   glLinkProgram(p); glUseProgram(p);
   static const float band[] = { -1,0.5f, 1,0.5f, -1,1, -1,1, 1,0.5f, 1,1 };
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, band);
   glEnableVertexAttribArray(0);
   glDrawArrays(GL_TRIANGLES, 0, 6);
   glFinish();

   void *map_data = NULL;
   uint32_t mstride = 0;
   void *ptr = gbm_bo_map(bo, 0, 0, W, H, GBM_BO_TRANSFER_READ, &mstride, &map_data);
   if (!ptr) { printf("gbm_bo_map failed\n"); return 1; }
   printf("mapped stride=%u\n", mstride);

   long bad = 0; int fx = -1, fy = -1;
   for (int y = 0; y < H; ++y) {
      unsigned char *row = (unsigned char *)ptr + (size_t)y * mstride;
      /* memory row 0 is the top of the displayed image; the red band was drawn
       * across the top in GL coordinates, which is the top on screen too */
      int red = y < H / 4;
      for (int x = 0; x < W; ++x) {
         unsigned char *q = row + x * 4;   /* XRGB little endian: B,G,R,X */
         int b = red ? 0 : 191, g = red ? 0 : 128, r = red ? 255 : 64;
         if (abs(q[0]-b) > 2 || abs(q[1]-g) > 2 || abs(q[2]-r) > 2) {
            if (!bad) { fx = x; fy = y; }
            bad++;
         }
      }
   }
   printf("imported renderbuffer: %ld/%ld wrong (%.1f%%)", bad, (long)W*H,
          100.0*bad/((double)W*H));
   if (bad) {
      unsigned char *q = (unsigned char *)ptr + (size_t)fy*mstride + fx*4;
      printf("  first %d,%d = b%d g%d r%d", fx, fy, q[0], q[1], q[2]);
   }
   printf("\n");
   gbm_bo_unmap(bo, map_data);
   return 0;
}
