/*
 * Reproduce weston's scanout path: a gbm surface, an EGL window surface,
 * render, swap, then lock the front buffer and read it back through gbm_bo_map.
 * That is the buffer the display scans out, so whatever lands here is what the
 * panel shows - no compositor and no screenshooter in the way.
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

static const char *vs_src =
  "attribute vec2 pos;varying vec2 p;void main(){p=pos*0.5+0.5;gl_Position=vec4(pos,0.0,1.0);}";
static const char *fs_src =
  "precision mediump float;varying vec2 p;void main(){gl_FragColor=vec4(p.x,p.y,0.25,1.0);}";

static GLuint sh(GLenum t, const char *s)
{
   GLuint x = glCreateShader(t);
   glShaderSource(x, 1, &s, NULL);
   glCompileShader(x);
   GLint ok = 0; glGetShaderiv(x, GL_COMPILE_STATUS, &ok);
   if (!ok) { char l[512]; glGetShaderInfoLog(x, 511, NULL, l); printf("compile: %s\n", l); }
   return x;
}

int main(int argc, char **argv)
{
   setbuf(stdout, NULL);
   int W = argc > 1 ? atoi(argv[1]) : 1366;
   int H = argc > 2 ? atoi(argv[2]) : 768;

   int fd = open("/dev/dri/card1", O_RDWR);
   if (fd < 0) { perror("open card1"); return 1; }

   struct gbm_device *gbm = gbm_create_device(fd);
   if (!gbm) { printf("gbm_create_device failed\n"); return 1; }

   struct gbm_surface *gs = gbm_surface_create(gbm, W, H, GBM_FORMAT_XRGB8888,
                                               GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
   if (!gs) { printf("gbm_surface_create failed\n"); return 1; }

   EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)gbm);
   EGLint maj, min;
   if (!eglInitialize(dpy, &maj, &min)) { printf("eglInitialize failed\n"); return 1; }
   eglBindAPI(EGL_OPENGL_ES_API);

   EGLint ca[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                   EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
   EGLConfig cfg; EGLint n = 0;
   /* pick a config whose native visual matches the gbm format */
   EGLConfig cfgs[32]; EGLint got = 0;
   eglChooseConfig(dpy, ca, cfgs, 32, &got);
   cfg = cfgs[0];
   for (int i = 0; i < got; i++) {
      EGLint id = 0;
      eglGetConfigAttrib(dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &id);
      if (id == GBM_FORMAT_XRGB8888) { cfg = cfgs[i]; break; }
   }

   EGLint cx[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cx);
   EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)gs, NULL);
   if (surf == EGL_NO_SURFACE) { printf("eglCreateWindowSurface failed 0x%x\n", eglGetError()); return 1; }
   if (!eglMakeCurrent(dpy, surf, surf, ctx)) { printf("makeCurrent failed 0x%x\n", eglGetError()); return 1; }
   printf("renderer: %s  %dx%d\n", glGetString(GL_RENDERER), W, H);

   GLuint p = glCreateProgram();
   glAttachShader(p, sh(GL_VERTEX_SHADER, vs_src));
   glAttachShader(p, sh(GL_FRAGMENT_SHADER, fs_src));
   glBindAttribLocation(p, 0, "pos");
   glLinkProgram(p);
   glUseProgram(p);

   static const GLfloat v[] = { -1,-1, 1,-1, -1,1,  1,-1, 1,1, -1,1 };
   glViewport(0, 0, W, H);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
   glEnableVertexAttribArray(0);

   int frames = argc > 3 ? atoi(argv[3]) : 1;
   struct gbm_bo *prev = NULL;
   for (int f = 0; f < frames; f++) {
      glClearColor(0, 0, 0, 1);
      glClear(GL_COLOR_BUFFER_BIT);
      glDrawArrays(GL_TRIANGLES, 0, 6);

      /* what weston's screenshooter does: glReadPixels before the swap */
      unsigned char q[4];
      int ry = H / 4;
      glReadPixels(W/2, ry, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q);
      int wantg = (int)(((H - 1 - ry) + 0.5) / H * 255.0 + 0.5); /* readpixels is bottom up */
      printf("frame %d: glReadPixels mid,%d -> r=%u g=%u (want g~%u) %s\n",
             f, ry, q[0], q[1], wantg,
             (abs((int)q[1] - wantg) <= 10) ? "ok" : "WRONG");

      /* the full frame readback weston's screenshooter performs */
      if (f == frames - 1) {
         unsigned char *fb = malloc((size_t)W * H * 4);
         glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, fb);
         int badrow = -1;
         for (int y = 0; y < H && badrow < 0; y++) {
            /*
             * glReadPixels is bottom up: its row 0 is the bottom of the
             * viewport, where the gradient's green is smallest. That is the
             * opposite of the scanout buffer checked below, which is top down.
             */
            int wg = (int)((y + 0.5) / H * 255.0 + 0.5);
            for (int x = 0; x < W; x += 89) {
               int wr = (int)((x + 0.5) / W * 255.0 + 0.5);
               unsigned char *px = fb + ((size_t)y * W + x) * 4;
               if (abs((int)px[0] - wr) > 10 || abs((int)px[1] - wg) > 10) { badrow = y; break; }
            }
         }
         printf("         full-frame glReadPixels: %s\n",
                badrow < 0 ? "every row correct" : "WRONG");
         if (badrow >= 0) printf("         first wrong readpixels row %d of %d\n", badrow, H);
         free(fb);
      }

      eglSwapBuffers(dpy, surf);
      if (prev) gbm_surface_release_buffer(gs, prev);
      prev = gbm_surface_lock_front_buffer(gs);
      printf("         front bo handle-ish %p stride=%u\n", (void *)prev,
             gbm_bo_get_stride(prev));
   }
   struct gbm_bo *bo = prev;
   if (0) bo = gbm_surface_lock_front_buffer(gs);
   if (!bo) { printf("lock_front_buffer failed\n"); return 1; }
   printf("front bo: %ux%u stride=%u\n", gbm_bo_get_width(bo), gbm_bo_get_height(bo),
          gbm_bo_get_stride(bo));

   uint32_t stride = 0; void *mapdata = NULL;
   void *ptr = gbm_bo_map(bo, 0, 0, W, H, GBM_BO_TRANSFER_READ, &stride, &mapdata);
   if (!ptr) { printf("gbm_bo_map failed\n"); return 1; }
   printf("mapped stride=%u\n", stride);

   /*
    * The shader writes green = p.y, and p.y is 0 at NDC y = -1, which GL puts
    * at the BOTTOM of the viewport. A scanout buffer is stored top down, so
    * row 0 is the top of the screen and must hold the LARGEST green. Getting
    * this backwards is exactly how an upside down display passes a test.
    */
   int bad = -1; int firstbadrow = -1;
   for (int y = 0; y < H; y++) {
      const unsigned char *row = (const unsigned char *)ptr + (size_t)y * stride;
      int wantg = (int)(((H - 1 - y) + 0.5) / H * 255.0 + 0.5);
      for (int x = 0; x < W; x += 97) {
         int wantr = (int)((x + 0.5) / W * 255.0 + 0.5);
         int r = row[x*4+2], g = row[x*4+1];   /* XRGB8888 little endian: B,G,R,X */
         if (abs(r - wantr) > 10 || abs(g - wantg) > 10) {
            if (firstbadrow < 0) { firstbadrow = y; bad = x; }
         }
      }
   }
   if (firstbadrow < 0)
      printf("RESULT: every row correct - the scanned out buffer is clean\n");
   else {
      const unsigned char *row = (const unsigned char *)ptr + (size_t)firstbadrow * stride;
      printf("RESULT: first wrong row %d of %d (x=%d got %u,%u want ~%u,%u)\n",
             firstbadrow, H, bad, row[bad*4+2], row[bad*4+1],
             (unsigned)((bad + 0.5) / W * 255.0 + 0.5),
             (unsigned)(((H - 1 - firstbadrow) + 0.5) / H * 255.0 + 0.5));
   }

   gbm_bo_unmap(bo, mapdata);
   gbm_surface_release_buffer(gs, bo);
   return firstbadrow < 0 ? 0 : 2;
}
