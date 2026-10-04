/*
 * grate fragment-shader conformance harness.
 *
 * Renders a full-viewport triangle per case and reads back the centre pixel.
 * Run once on grate and once on softpipe (LIBGL_ALWAYS_SOFTWARE=1); the two
 * outputs are diffed by fptest.sh, so softpipe is the reference oracle.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

struct testcase {
   const char *name;
   const char *fs;
   int has_uv;          /* feed a varying */
   int has_tex;         /* bind a 2x2 texture */
};

static const char *vs_plain =
   "attribute vec4 pos;\n"
   "void main() { gl_Position = pos; }\n";

static const char *vs_uv =
   "attribute vec4 pos;\n"
   "varying vec2 uv;\n"
   "void main() { gl_Position = pos; uv = pos.xy * 0.5 + 0.5; }\n";

static const struct testcase tests[] = {
   { "const_red",   "void main(){ gl_FragColor = vec4(1.0,0.0,0.0,1.0); }", 0, 0 },
   { "const_white", "void main(){ gl_FragColor = vec4(1.0); }", 0, 0 },
   { "const_grey",  "void main(){ gl_FragColor = vec4(0.5,0.5,0.5,1.0); }", 0, 0 },
   { "mul",         "void main(){ gl_FragColor = vec4(1.0,1.0,0.0,1.0) * vec4(0.5); }", 0, 0 },
   { "add",         "void main(){ gl_FragColor = vec4(0.25,0.0,0.0,0.5) + vec4(0.25,0.0,0.0,0.5); }", 0, 0 },
   { "mad",         "void main(){ gl_FragColor = vec4(0.5,0.5,0.0,1.0) * vec4(0.5) + vec4(0.25,0.0,0.5,0.0); }", 0, 0 },
   { "min",         "void main(){ gl_FragColor = min(vec4(0.75,0.25,1.0,1.0), vec4(0.5)); }", 0, 0 },
   { "max",         "void main(){ gl_FragColor = max(vec4(0.75,0.25,0.0,1.0), vec4(0.5)); }", 0, 0 },
   { "swizzle",     "void main(){ vec4 c = vec4(1.0,0.5,0.25,1.0); gl_FragColor = c.zyxw; }", 0, 0 },
   { "saturate",    "void main(){ gl_FragColor = clamp(vec4(2.0,-1.0,0.5,1.0), 0.0, 1.0); }", 0, 0 },
   { "two_inst",    "void main(){ gl_FragColor.xy = vec2(1.0,0.0); gl_FragColor.zw = vec2(0.0,1.0); }", 0, 0 },
   { "three_inst",  "void main(){ gl_FragColor.x = 1.0; gl_FragColor.y = 0.0; gl_FragColor.zw = vec2(0.5,1.0); }", 0, 0 },
   /* ops with no ALU opcode of their own: lowered to the SFU or to a
    * sequence. uv keeps the GLSL compiler from folding them away. */
   { "seq",         "varying vec2 uv;\nvoid main(){ gl_FragColor = vec4(equal(vec4(uv,0.0,1.0), vec4(0.5,0.5,0.0,1.0))); }", 1, 0 },
   { "sne",         "varying vec2 uv;\nvoid main(){ gl_FragColor = vec4(notEqual(vec4(uv,0.0,1.0), vec4(0.5,0.5,0.0,1.0))); }", 1, 0 },
   { "lrp",         "varying vec2 uv;\nvoid main(){ gl_FragColor = mix(vec4(1.0,0.0,0.0,1.0), vec4(0.0,1.0,0.5,1.0), uv.x); }", 1, 0 },
   { "sfu_frc",         "varying vec2 uv;\nvoid main(){ gl_FragColor = vec4(fract(uv*3.0), 0.0, 1.0); }", 1, 0 },
   { "sfu_flr",         "varying vec2 uv;\nvoid main(){ gl_FragColor = vec4(floor(uv*3.0)*0.25, 0.0, 1.0); }", 1, 0 },
   { "sfu_rcp",         "varying vec2 uv;\nvoid main(){ gl_FragColor = vec4(1.0/(uv.x+1.5), 1.0/(uv.y+2.0), 0.0, 1.0); }", 1, 0 },
   { "sfu_rsq",         "varying vec2 uv;\nvoid main(){ gl_FragColor = vec4(inversesqrt(uv.x+1.5)*0.5, 0.0, 0.0, 1.0); }", 1, 0 },
   { "sfu_sqrt",       "varying vec2 uv;\nvoid main(){ gl_FragColor = vec4(sqrt(uv.x+0.25)*0.5, 0.0, 0.0, 1.0); }", 1, 0 },
   { "sfu_frc_add",     "varying vec2 uv;\nvoid main(){ gl_FragColor = vec4(fract(uv.x+1.25), 0.0, 0.0, 1.0); }", 1, 0 },
   { "sfu_rcp_scaled",  "varying vec2 uv;\nvoid main(){ gl_FragColor = vec4((1.0/(uv.x+1.5))*0.5, 0.0, 0.0, 1.0); }", 1, 0 },
   { "sfu_rcp_tmp",     "varying vec2 uv;\nvoid main(){ float r = 1.0/(uv.x+1.5); gl_FragColor = vec4(r, r, 0.0, 1.0); }", 1, 0 },
   /* does a value written to a temporary survive into the next instruction? */
   { "tmp_chain",   "varying vec2 uv;\nvoid main(){ float t = uv.x * 0.5; gl_FragColor = vec4(t + 0.25, 0.0, 0.0, 1.0); }", 1, 0 },
   { "tmp_twice",   "varying vec2 uv;\nvoid main(){ float t = uv.x * 0.5; gl_FragColor = vec4(t, t, 0.0, 1.0); }", 1, 0 },
   { "tmp_deep",    "varying vec2 uv;\nvoid main(){ float a = uv.x*0.5; float b = a+0.1; float c = b*2.0; gl_FragColor = vec4(c, 0.0, 0.0, 1.0); }", 1, 0 },
   /* two separate instructions, each writing one output component */
   { "two_out_same","varying vec2 uv;\nvoid main(){ gl_FragColor.x = uv.x*0.5; gl_FragColor.y = uv.x*0.25; gl_FragColor.zw = vec2(0.0,1.0); }", 1, 0 },
   { "two_out_diff","varying vec2 uv;\nvoid main(){ gl_FragColor.x = uv.x*0.5; gl_FragColor.y = uv.y*0.25; gl_FragColor.zw = vec2(0.0,1.0); }", 1, 0 },
   { "sfu_late",    "varying vec2 uv;\nvoid main(){ gl_FragColor.x = uv.x*0.5; gl_FragColor.y = 1.0/(uv.x+1.5); gl_FragColor.zw = vec2(0.0,1.0); }", 1, 0 },
   { "sfu_first",   "varying vec2 uv;\nvoid main(){ gl_FragColor.x = 1.0/(uv.x+1.5); gl_FragColor.y = uv.x*0.5; gl_FragColor.zw = vec2(0.0,1.0); }", 1, 0 },
   { "vmul1",       "varying vec2 uv;\nvoid main(){ gl_FragColor = vec4(uv.x*0.5, 0.0, 0.0, 1.0); }", 1, 0 },
   { "vmul2",       "varying vec2 uv;\nvoid main(){ gl_FragColor = vec4(uv.x*0.5, uv.x*0.25, 0.0, 1.0); }", 1, 0 },
   { "vmulxy",      "varying vec2 uv;\nvoid main(){ gl_FragColor = vec4(uv.x*0.5, uv.y*0.5, 0.0, 1.0); }", 1, 0 },
   { "varying",     "varying vec2 uv;\nvoid main(){ gl_FragColor = vec4(uv, 0.0, 1.0); }", 1, 0 },
   { "varying_x",   "varying vec2 uv;\nvoid main(){ gl_FragColor = vec4(uv.x, 0.0, 0.0, 1.0); }", 1, 0 },
   { "texture",     "varying vec2 uv;\nuniform sampler2D t;\nvoid main(){ gl_FragColor = texture2D(t, uv); }", 1, 1 },
   { "tex_mul",     "varying vec2 uv;\nuniform sampler2D t;\nvoid main(){ gl_FragColor = texture2D(t, uv) * vec4(0.5); }", 1, 1 },
   { "tex_npot",    "varying vec2 uv;\nuniform sampler2D t;\nvoid main(){ gl_FragColor = texture2D(t, uv); }", 1, 2 },
   { "tex_5x4",     "varying vec2 uv;\nuniform sampler2D t;\nvoid main(){ gl_FragColor = texture2D(t, uv); }", 1, 7 },
   { "tex_8x3",     "varying vec2 uv;\nuniform sampler2D t;\nvoid main(){ gl_FragColor = texture2D(t, uv); }", 1, 8 },
   { "tex_100x50",  "varying vec2 uv;\nuniform sampler2D t;\nvoid main(){ gl_FragColor = texture2D(t, uv); }", 1, 9 },
   { "tex_806x491", "varying vec2 uv;\nuniform sampler2D t;\nvoid main(){ gl_FragColor = texture2D(t, uv); }", 1, 10 },
   { "tex_1366x32", "varying vec2 uv;\nuniform sampler2D t;\nvoid main(){ gl_FragColor = texture2D(t, uv); }", 1, 11 },
   { "tex_c00",     "uniform sampler2D t;\nvoid main(){ gl_FragColor = texture2D(t, vec2(0.07,0.07)); }", 0, 1 },
   { "tex_c10",     "uniform sampler2D t;\nvoid main(){ gl_FragColor = texture2D(t, vec2(0.93,0.07)); }", 0, 1 },
   { "tex_c01",     "uniform sampler2D t;\nvoid main(){ gl_FragColor = texture2D(t, vec2(0.07,0.93)); }", 0, 1 },
   { "tex_c11",     "uniform sampler2D t;\nvoid main(){ gl_FragColor = texture2D(t, vec2(0.93,0.93)); }", 0, 1 },
   { "tex_big",     "varying vec2 uv;\nuniform sampler2D t;\nvoid main(){ gl_FragColor = texture2D(t, uv); }", 1, 3 },
   { "tex_1366",    "varying vec2 uv;\nuniform sampler2D t;\nvoid main(){ gl_FragColor = texture2D(t, uv); }", 1, 6 },
   { "tex_sub",     "varying vec2 uv;\nuniform sampler2D t;\nvoid main(){ gl_FragColor = texture2D(t, uv); }", 1, 4 },
   { "tex_500",     "varying vec2 uv;\nuniform sampler2D t;\nvoid main(){ gl_FragColor = texture2D(t, uv); }", 1, 5 },
};

static GLuint compile(GLenum type, const char *src, char *log, size_t logsz)
{
   GLuint s = glCreateShader(type);
   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   GLint ok = 0;
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) { glGetShaderInfoLog(s, logsz - 1, NULL, log); return 0; }
   return s;
}

int main(void)
{
  /* Mesa built with the x11 platform makes EGL_DEFAULT_DISPLAY mean X11,
   * which is not there over ssh. Ask for surfaceless unless told otherwise. */
  setenv("EGL_PLATFORM", "surfaceless", 0);
   setbuf(stdout, NULL);
   EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   EGLint maj, min;
   if (!eglInitialize(dpy, &maj, &min)) { printf("eglInitialize failed\n"); return 1; }
   eglBindAPI(EGL_OPENGL_ES_API);
   EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                   EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
   EGLConfig cfg; EGLint n = 0;
   eglChooseConfig(dpy, ca, &cfg, 1, &n);
   EGLint cx[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cx);
   EGLint pb[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
   EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb);
   eglMakeCurrent(dpy, surf, surf, ctx);
   printf("# renderer: %s\n", glGetString(GL_RENDERER));

   GLuint tex = 0;
   glGenTextures(1, &tex);
   glBindTexture(GL_TEXTURE_2D, tex);
   {
      /* 8x8 so the natural pitch (32B) needs no alignment padding */
      static unsigned char px[8*8*4];
      for (int y = 0; y < 8; y++)
         for (int x = 0; x < 8; x++) {
            unsigned char *p = px + (y*8 + x)*4;
            /* gradient: red tracks x, green tracks y, so a stride error shows */
            p[0] = x * 32; p[1] = y * 32; p[2] = 64; p[3] = 255;
         }
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   }

   /* 5x3: pitch 20 bytes, deliberately not 32-byte aligned */
   GLuint tex_npot = 0;
   glGenTextures(1, &tex_npot);
   glBindTexture(GL_TEXTURE_2D, tex_npot);
   {
      static unsigned char np[5*3*4];
      for (int i = 0; i < 5*3; i++) { np[i*4+0]=32; np[i*4+1]=192; np[i*4+2]=255; np[i*4+3]=255; }
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 5, 3, 0, GL_RGBA, GL_UNSIGNED_BYTE, np);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   }

   /* 250x250, matching weston-flower's surface size */
   GLuint tex_big = 0;
   glGenTextures(1, &tex_big);
   glBindTexture(GL_TEXTURE_2D, tex_big);
   {
      static unsigned char bg[250*250*4];
      for (int y = 0; y < 250; y++)
         for (int x = 0; x < 250; x++) {
            unsigned char *p = bg + (y*250 + x)*4;
            p[0] = x; p[1] = y; p[2] = 64; p[3] = 255;
         }
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 250, 250, 0, GL_RGBA, GL_UNSIGNED_BYTE, bg);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   }

   /* 250x250 uploaded whole, then a sub-rectangle replaced, as weston does
    * when it re-uploads only the damaged part of a client surface */
   GLuint tex_sub = 0;
   glGenTextures(1, &tex_sub);
   glBindTexture(GL_TEXTURE_2D, tex_sub);
   {
      static unsigned char base[250*250*4];
      for (int i = 0; i < 250*250; i++) { base[i*4+0]=10; base[i*4+1]=10; base[i*4+2]=10; base[i*4+3]=255; }
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 250, 250, 0, GL_RGBA, GL_UNSIGNED_BYTE, base);

      /* replace a 100x100 patch at (75,75) with a distinctive colour */
      static unsigned char patch[100*100*4];
      for (int i = 0; i < 100*100; i++) { patch[i*4+0]=240; patch[i*4+1]=32; patch[i*4+2]=160; patch[i*4+3]=255; }
      glTexSubImage2D(GL_TEXTURE_2D, 0, 75, 75, 100, 100, GL_RGBA, GL_UNSIGNED_BYTE, patch);

      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   }

   /* 500x400, the size weston-image's toplevel surface uses: 500*4 = 2000
    * bytes per row, which is not a multiple of 32 */
   GLuint tex_500 = 0;
   glGenTextures(1, &tex_500);
   glBindTexture(GL_TEXTURE_2D, tex_500);
   {
      static unsigned char b5[500*400*4];
      for (int y = 0; y < 400; y++)
         for (int x = 0; x < 500; x++) {
            unsigned char *p = b5 + (y*500 + x)*4;
            p[0] = (x*255)/500; p[1] = (y*255)/400; p[2] = 64; p[3] = 255;
         }
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 500, 400, 0, GL_RGBA, GL_UNSIGNED_BYTE, b5);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   }

   /* 1366x768: weston's desktop background surface */
   GLuint tex_1366 = 0;
   glGenTextures(1, &tex_1366);
   glBindTexture(GL_TEXTURE_2D, tex_1366);
   {
      unsigned char *b = malloc(1366*768*4);
      for (int y = 0; y < 768; y++)
         for (int x = 0; x < 1366; x++) {
            unsigned char *p = b + (y*1366 + x)*4;
            p[0] = (x*255)/1366; p[1] = (y*255)/768; p[2] = 64; p[3] = 255;
         }
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1366, 768, 0, GL_RGBA, GL_UNSIGNED_BYTE, b);
      free(b);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   }

   /* solid-colour textures at assorted power-of-two / non-power-of-two sizes */
   GLuint tex_alt[5] = {0,0,0,0,0};
   {
      static const int dims[5][2] = { {5,4}, {8,3}, {100,50}, {806,491}, {1366,32} };
      glGenTextures(5, tex_alt);
      for (int i = 0; i < 5; i++) {
         int w = dims[i][0], h = dims[i][1];
         unsigned char *b = malloc(w*h*4);
         for (int k = 0; k < w*h; k++) { b[k*4+0]=32; b[k*4+1]=192; b[k*4+2]=255; b[k*4+3]=255; }
         glBindTexture(GL_TEXTURE_2D, tex_alt[i]);
         glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, b);
         free(b);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      }
   }

   static const GLfloat verts[] = { -1.0f,-1.0f, 3.0f,-1.0f, -1.0f,3.0f };

   const char *only = getenv("FPTEST_ONLY");
   for (unsigned i = 0; i < sizeof(tests)/sizeof(tests[0]); i++) {
      const struct testcase *t = &tests[i];
      if (only && strcmp(only, t->name) != 0) continue;
      char log[512] = {0};
      GLuint vs = compile(GL_VERTEX_SHADER, t->has_uv ? vs_uv : vs_plain, log, sizeof(log));
      if (!vs) { printf("%s VS_COMPILE_FAIL %s\n", t->name, log); continue; }
      char fsrc[2048];
      snprintf(fsrc, sizeof(fsrc), "precision mediump float;\n%s", t->fs);
      GLuint fs = compile(GL_FRAGMENT_SHADER, fsrc, log, sizeof(log));
      if (!fs) { printf("%s FS_COMPILE_FAIL %s\n", t->name, log); continue; }

      GLuint prog = glCreateProgram();
      glAttachShader(prog, vs);
      glAttachShader(prog, fs);
      glBindAttribLocation(prog, 0, "pos");
      glLinkProgram(prog);
      GLint ok = 0;
      glGetProgramiv(prog, GL_LINK_STATUS, &ok);
      if (!ok) { printf("%s LINK_FAIL\n", t->name); continue; }
      glUseProgram(prog);

      if (t->has_tex) {
         glActiveTexture(GL_TEXTURE0);
         glBindTexture(GL_TEXTURE_2D, t->has_tex == 2 ? tex_npot :
                                        t->has_tex == 3 ? tex_big :
                                        t->has_tex == 7 ? tex_alt[0] :
                                        t->has_tex == 8 ? tex_alt[1] :
                                        t->has_tex == 9 ? tex_alt[2] :
                                        t->has_tex == 10 ? tex_alt[3] :
                                        t->has_tex == 11 ? tex_alt[4] :
                                        t->has_tex == 6 ? tex_1366 :
                                        t->has_tex == 4 ? tex_sub :
                                        t->has_tex == 5 ? tex_500 : tex);
         glUniform1i(glGetUniformLocation(prog, "t"), 0);
      }

      glViewport(0, 0, 64, 64);
      glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts);
      glEnableVertexAttribArray(0);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      glFinish();

      GLenum err = glGetError();
      if (t->has_uv) {
         /* a shear shows up as a wrong sample away from the centre */
         static const int pts[9][2] = {
            {8,8},{32,8},{56,8}, {8,32},{32,32},{56,32}, {8,56},{32,56},{56,56} };
         for (int k = 0; k < 9; k++) {
            unsigned char q[4] = {0};
            glReadPixels(pts[k][0], pts[k][1], 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q);
            printf("%s@%d %u,%u,%u,%u err=0x%x\n", t->name, k, q[0], q[1], q[2], q[3], err);
         }
      } else {
         unsigned char px[4] = {0};
         glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
         printf("%s %u,%u,%u,%u err=0x%x\n", t->name, px[0], px[1], px[2], px[3], err);
      }
      glDeleteProgram(prog);
      glDeleteShader(vs);
      glDeleteShader(fs);
   }
   return 0;
}
