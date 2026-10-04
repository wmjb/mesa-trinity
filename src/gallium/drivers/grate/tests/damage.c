/*
 * Weston repaints only what changed: it asks EGL how old the buffer it is
 * about to draw into is, and redraws just the damage when that buffer already
 * holds a recent frame. This walks the same path - full frame, then scissored
 * repaints - and checks the parts nobody redrew still hold what was put there.
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

#define W 1366
#define H 768

static const char *vs_src =
  "attribute vec2 pos;varying vec2 p;void main(){p=pos*0.5+0.5;gl_Position=vec4(pos,0.0,1.0);}";
static const char *fs_src =
  "precision mediump float;varying vec2 p;uniform vec4 c;void main(){gl_FragColor=c;}";

static GLuint sh(GLenum t, const char *s)
{
   GLuint x = glCreateShader(t);
   glShaderSource(x, 1, &s, NULL);
   glCompileShader(x);
   return x;
}

int main(void)
{
   int fd = open("/dev/dri/renderD128", O_RDWR);
   struct gbm_device *gbm = gbm_create_device(fd);
   struct gbm_surface *gs = gbm_surface_create(gbm, W, H, GBM_FORMAT_XRGB8888,
                                               GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
   if (!gs) { printf("gbm_surface_create failed\n"); return 1; }

   EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)gbm);
   eglInitialize(dpy, NULL, NULL);
   eglBindAPI(EGL_OPENGL_ES_API);

   EGLint ca[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                   EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                   EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
   EGLConfig cfgs[32]; EGLint got;
   eglChooseConfig(dpy, ca, cfgs, 32, &got);
   EGLConfig cfg = cfgs[0];
   for (int i = 0; i < got; i++) {
      EGLint id;
      eglGetConfigAttrib(dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &id);
      if (id == GBM_FORMAT_XRGB8888) { cfg = cfgs[i]; break; }
   }

   EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xa);
   EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)gs, NULL);
   eglMakeCurrent(dpy, surf, surf, ctx);

   PFNEGLQUERYSURFACEPROC qs = (void *)eglGetProcAddress("eglQuerySurface");

   GLuint p = glCreateProgram();
   glAttachShader(p, sh(GL_VERTEX_SHADER, vs_src));
   glAttachShader(p, sh(GL_FRAGMENT_SHADER, fs_src));
   glBindAttribLocation(p, 0, "pos");
   glLinkProgram(p);
   glUseProgram(p);

   static const float quad[] = { -1,-1, 1,-1, -1,1, -1,1, 1,-1, 1,1 };
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
   glEnableVertexAttribArray(0);
   glViewport(0, 0, W, H);

   unsigned char *px = malloc((size_t)W * H * 4);

   for (int f = 0; f < 6; ++f) {
      EGLint age = -1;
      qs(dpy, surf, EGL_BUFFER_AGE_EXT, &age);

      if (f < 3) {
         /* fill the whole buffer, the way weston does until every buffer in
          * the set has been drawn at least once */
         glDisable(GL_SCISSOR_TEST);
         glUniform4f(glGetUniformLocation(p, "c"), 0.25f, 0.5f, 0.75f, 1.0f);
         glDrawArrays(GL_TRIANGLES, 0, 6);
      } else {
         /* repaint only the damage */
         glEnable(GL_SCISSOR_TEST);
         glScissor(100, 100, 200, 100);
         glUniform4f(glGetUniformLocation(p, "c"), 1.0f, 0.0f, 0.0f, 1.0f);
         glDrawArrays(GL_TRIANGLES, 0, 6);
         glDisable(GL_SCISSOR_TEST);
      }

      glFinish();
      memset(px, 0xAB, (size_t)W * H * 4);
      glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);

      long bad = 0; int fx = -1, fy = -1;
      for (int y = 0; y < H; ++y)
         for (int x = 0; x < W; ++x) {
            int inside = f >= 3 && x >= 100 && x < 300 && y >= 100 && y < 200;
            unsigned char *q = px + ((size_t)y * W + x) * 4;
            int r = inside ? 255 : 64, g = inside ? 0 : 128, b = inside ? 0 : 191;
            if (abs(q[0]-r) > 2 || abs(q[1]-g) > 2 || abs(q[2]-b) > 2) {
               if (!bad) { fx = x; fy = y; }
               bad++;
            }
         }
      printf("frame %d age=%d: %ld/%ld wrong", f, age, bad, (long)W*H);
      if (bad) printf("  first %d,%d = %d,%d,%d", fx, fy,
                      px[((size_t)fy*W+fx)*4], px[((size_t)fy*W+fx)*4+1],
                      px[((size_t)fy*W+fx)*4+2]);
      printf("\n");

      eglSwapBuffers(dpy, surf);
      struct gbm_bo *bo = gbm_surface_lock_front_buffer(gs);
      if (bo) gbm_surface_release_buffer(gs, bo);
   }
   return 0;
}
