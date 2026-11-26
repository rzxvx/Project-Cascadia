// SPDX-License-Identifier: MIT
/*
 * g9p-glcheck: OpenGL ES 2.0 features through Cascadia's Gallium driver
 * (g9p), each drawn into a framebuffer object and every pixel compared with
 * the CPU (edges left out). EGL surfaceless.
 *
 *   g9p-glcheck uniform [W H]        a matrix (vertex) and a tint (fragment);
 *                                    uniform-vs / uniform-fs: one stage's only,
 *                                    uniform-vs4: the transform from one vec4
 *                                    (G9P_TEST_VS=source: another vertex shader
 *                                    with uniform mat4 mvp doing the same)
 *   g9p-glcheck tex TW TH [W H]      a TW x TH texture over the target, nearest
 *   g9p-glcheck texmod TW TH [W H]   the same times a uniform colour
 *   g9p-glcheck depth FUNC [W H]     two overlapping triangles at different
 *                                    depths (less, lequal, greater, gequal,
 *                                    equal, notequal, always, never; nowrite)
 *   g9p-glcheck cull FACE [W H]      a counter-clockwise and a clockwise
 *                                    triangle, culling back / front / none
 *                                    (frontcw: front faces clockwise, back culled)
 *   g9p-glcheck stencil [W H]        a triangle writes 1 into the stencil, a
 *                                    quad then draws where it is 1 (and 0)
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIE(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } while (0)

static GLuint
shader(GLenum type, const char *src)
{
   GLuint s = glCreateShader(type);
   GLint ok;
   char log[1024];

   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      glGetShaderInfoLog(s, sizeof(log), NULL, log);
      DIE("shader: %s", log);
   }
   return s;
}

static GLuint
program(const char *vs, const char *fs)
{
   GLuint p = glCreateProgram();

   glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
   glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
   glBindAttribLocation(p, 0, "pos");
   glBindAttribLocation(p, 1, "attr");
   glLinkProgram(p);
   glUseProgram(p);
   return p;
}

static void
context(void)
{
   PFNEGLGETPLATFORMDISPLAYEXTPROC gpd =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
   static const EGLint cattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLDisplay dpy;
   EGLContext ctx;
   EGLint maj, min;

   if (!gpd || !(dpy = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL)) ||
       !eglInitialize(dpy, &maj, &min))
      DIE("no surfaceless EGL display");
   eglBindAPI(EGL_OPENGL_ES_API);
   if (!(ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, cattr)) ||
       !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
      DIE("context: %#x", eglGetError());
}

static void
target(int w, int h)
{
   GLuint fbo, rb;

   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glGenRenderbuffers(1, &rb);
   glBindRenderbuffer(GL_RENDERBUFFER, rb);
   glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, w, h);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      DIE("framebuffer incomplete");
   glViewport(0, 0, w, h);
}

static void
attribs(const float *v, int n, int stride_floats)
{
   GLuint buf;

   glGenBuffers(1, &buf);
   glBindBuffer(GL_ARRAY_BUFFER, buf);
   glBufferData(GL_ARRAY_BUFFER, n * stride_floats * 4, v, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride_floats * 4, (void *)0);
   glVertexAttribPointer(1, stride_floats - 2, GL_FLOAT, GL_FALSE, stride_floats * 4, (void *)8);
   glEnableVertexAttribArray(0);
   glEnableVertexAttribArray(1);
}

/* the texel of a TW x TH pattern: every one different */
static void
texel(int x, int y, unsigned char *c)
{
   c[0] = (x * 37 + 11) & 255;
   c[1] = (y * 53 + 7) & 255;
   c[2] = ((x ^ y) * 29) & 255;
   c[3] = 255 - ((x + y) & 63);
}

static int bad, total;

static void
check(int x, int y, const unsigned char *got, const double *want)
{
   int off = 0;

   total++;
   for (int k = 0; k < 4; k++)
      if (abs(got[k] - (int)lround(fmin(fmax(want[k], 0), 1) * 255)) > 3)
         off = 1;
   if (off && bad++ < 5)
      printf("  (%d, %d): got %d %d %d %d want %.0f %.0f %.0f %.0f\n", x, y, got[0], got[1], got[2], got[3],
             want[0] * 255, want[1] * 255, want[2] * 255, want[3] * 255);
}

/* the barycentric coordinates of pixel centre (x, y) in a triangle of
 * clip-space corners v[i * stride], -1 when outside, 0 near an edge */
static int
inside(const float *v, int stride, int w, int h, int x, int y, double *b)
{
   double sx[3], sy[3];

   for (int i = 0; i < 3; i++) {
      sx[i] = (v[i * stride] + 1) * w / 2;
      sy[i] = (v[i * stride + 1] + 1) * h / 2;
   }
   double d = (sy[1] - sy[2]) * (sx[0] - sx[2]) + (sx[2] - sx[1]) * (sy[0] - sy[2]);
   double cx = x + 0.5, cy = y + 0.5;
   b[0] = ((sy[1] - sy[2]) * (cx - sx[2]) + (sx[2] - sx[1]) * (cy - sy[2])) / d;
   b[1] = ((sy[2] - sy[0]) * (cx - sx[2]) + (sx[0] - sx[2]) * (cy - sy[2])) / d;
   b[2] = 1 - b[0] - b[1];
   double m = fmin(b[0], fmin(b[1], b[2]));
   if (fabs(m) < 2.0 / (w < h ? w : h))
      return 0;
   return m > 0 ? 1 : -1;
}

static const char *flat_vs = "attribute vec4 pos;\nattribute vec4 attr;\nvarying vec4 v;\n"
                             "void main() { v = attr; gl_Position = pos; }\n";
static const char *flat_fs = "precision mediump float;\nvarying vec4 v;\nvoid main() { gl_FragColor = v; }\n";

/* depth / cull / stencil: triangles of 7 floats (x, y, z, r, g, b, a) */
static void
draw7(const float *v, int n)
{
   GLuint buf;

   glGenBuffers(1, &buf);
   glBindBuffer(GL_ARRAY_BUFFER, buf);
   glBufferData(GL_ARRAY_BUFFER, n * 28, v, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 28, (void *)0);
   glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 28, (void *)12);
   glEnableVertexAttribArray(0);
   glEnableVertexAttribArray(1);
   glDrawArrays(GL_TRIANGLES, 0, n);
}

static int
depth_pass(const char *f, double z, double stored)
{
   if (!strcmp(f, "less")) return z < stored;
   if (!strcmp(f, "lequal")) return z <= stored;
   if (!strcmp(f, "greater")) return z > stored;
   if (!strcmp(f, "gequal")) return z >= stored;
   if (!strcmp(f, "equal")) return z == stored;
   if (!strcmp(f, "notequal")) return z != stored;
   if (!strcmp(f, "never")) return 0;
   return 1;
}

static GLenum
depth_func(const char *f)
{
   static const struct { const char *n; GLenum e; } m[] = {
      { "less", GL_LESS }, { "lequal", GL_LEQUAL }, { "greater", GL_GREATER }, { "gequal", GL_GEQUAL },
      { "equal", GL_EQUAL }, { "notequal", GL_NOTEQUAL }, { "never", GL_NEVER }, { "always", GL_ALWAYS },
      { "nowrite", GL_LESS } };
   for (unsigned i = 0; i < sizeof(m) / sizeof(m[0]); i++)
      if (!strcmp(f, m[i].n))
         return m[i].e;
   DIE("depth function %s?", f);
}

static int
zs_main(const char *mode, const char *arg, int w, int h)
{
   unsigned char *px = malloc(4 * w * h);
   static const double clear[4] = { 0.25, 0.5, 0.75, 1 };
   GLuint fbo, rb, zs;

   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glGenRenderbuffers(1, &rb);
   glBindRenderbuffer(GL_RENDERBUFFER, rb);
   glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, w, h);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
   glGenRenderbuffers(1, &zs);
   glBindRenderbuffer(GL_RENDERBUFFER, zs);
   glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, w, h);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, zs);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, zs);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      DIE("framebuffer with depth / stencil incomplete");
   glViewport(0, 0, w, h);
   program(flat_vs, flat_fs);
   glClearColor(clear[0], clear[1], clear[2], clear[3]);

   if (!strcmp(mode, "depth")) {
      /* red at z 0.2 (depth 0.6), then green at z -0.4 .. 0.8 (a slope:
       * nearer on the left, further on the right) over it */
      static const float t[6][7] = {
         { -0.9f, -0.9f, 0.2f, 1, 0, 0, 1 }, { 0.6f, -0.9f, 0.2f, 1, 0, 0, 1 }, { -0.9f, 0.7f, 0.2f, 1, 0, 0, 1 },
         { -0.6f, -0.7f, -0.4f, 0, 1, 0, 1 }, { 0.9f, -0.7f, 0.8f, 0, 1, 0, 1 }, { 0.2f, 0.9f, 0.2f, 0, 1, 0, 1 } };
      const int greater = !strncmp(arg, "greater", 7) || !strcmp(arg, "gequal");
      const double zc = greater ? 0.0 : 1.0;
      glClearDepthf(zc);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      glEnable(GL_DEPTH_TEST);
      glDepthFunc(depth_func(arg));
      glDepthMask(strcmp(arg, "nowrite") != 0);
      draw7(&t[0][0], 6);
      glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
      for (int y = 0; y < h; y++)
         for (int x = 0; x < w; x++) {
            static const double red[4] = { 1, 0, 0, 1 }, green[4] = { 0, 1, 0, 1 };
            double b0[3], b1[3], want[4], stored = zc, zz;
            int i0 = inside(&t[0][0], 7, w, h, x, y, b0), i1 = inside(&t[3][0], 7, w, h, x, y, b1);
            const double *c = clear;
            if (!i0 || !i1)
               continue;
            if (i0 > 0 && depth_pass(arg, zz = 0.6, stored)) {
               c = red;
               if (strcmp(arg, "nowrite"))
                  stored = zz;
            }
            if (i1 > 0) {
               zz = (b1[0] * -0.4 + b1[1] * 0.8 + b1[2] * 0.2) * 0.5 + 0.5;
               if (fabs(zz - stored) < 0.01 || fabs(zz - zc) < 0.01)
                  continue;                         /* the depths too close to call */
               if (depth_pass(arg, zz, stored))
                  c = green;
            }
            memcpy(want, c, sizeof(want));
            check(x, y, px + 4 * (y * w + x), want);
         }
   } else if (!strcmp(mode, "cull")) {
      /* left: counter-clockwise (red), right: clockwise (green), in GL's
       * window coordinates */
      static const float t[6][7] = {
         { -0.9f, -0.8f, 0, 1, 0, 0, 1 }, { -0.1f, -0.8f, 0, 1, 0, 0, 1 }, { -0.5f, 0.8f, 0, 1, 0, 0, 1 },
         { 0.1f, -0.8f, 0, 0, 1, 0, 1 }, { 0.5f, 0.8f, 0, 0, 1, 0, 1 }, { 0.9f, -0.8f, 0, 0, 1, 0, 1 } };
      int keep_ccw = 1, keep_cw = 1;
      glClear(GL_COLOR_BUFFER_BIT);
      if (strcmp(arg, "none")) {
         glEnable(GL_CULL_FACE);
         glFrontFace(!strcmp(arg, "frontcw") ? GL_CW : GL_CCW);
         glCullFace(!strcmp(arg, "front") ? GL_FRONT : GL_BACK);
         if (!strcmp(arg, "back"))
            keep_cw = 0;
         else if (!strcmp(arg, "front"))
            keep_ccw = 0;
         else
            keep_ccw = 0;                                   /* frontcw: back = counter-clockwise */
      }
      draw7(&t[0][0], 6);
      glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
      for (int y = 0; y < h; y++)
         for (int x = 0; x < w; x++) {
            double b[3], want[4];
            int i0 = inside(&t[0][0], 7, w, h, x, y, b), i1 = inside(&t[3][0], 7, w, h, x, y, b);
            if (!i0 || !i1)
               continue;
            const double *c = i0 > 0 && keep_ccw ? (double[]){ 1, 0, 0, 1 }
                              : i1 > 0 && keep_cw ? (double[]){ 0, 1, 0, 1 } : clear;
            memcpy(want, c, sizeof(want));
            check(x, y, px + 4 * (y * w + x), want);
         }
   } else {
      /* stencil: a triangle writes 1 (red), then a quad over everything:
       * green where the stencil is 1, then blue where it is 0 */
      static const float tri[3][7] = { { -0.8f, -0.8f, 0, 1, 0, 0, 1 }, { 0.8f, -0.6f, 0, 1, 0, 0, 1 },
                                       { -0.2f, 0.8f, 0, 1, 0, 0, 1 } };
      static const float qg[6][7] = {
         { -1, -1, 0, 0, 1, 0, 1 }, { 1, -1, 0, 0, 1, 0, 1 }, { 1, 1, 0, 0, 1, 0, 1 },
         { -1, -1, 0, 0, 1, 0, 1 }, { 1, 1, 0, 0, 1, 0, 1 }, { -1, 1, 0, 0, 1, 0, 1 } };
      static const float qb[6][7] = {
         { -1, -1, 0, 0, 0, 1, 1 }, { 1, -1, 0, 0, 0, 1, 1 }, { 1, 1, 0, 0, 0, 1, 1 },
         { -1, -1, 0, 0, 0, 1, 1 }, { 1, 1, 0, 0, 0, 1, 1 }, { -1, 1, 0, 0, 0, 1, 1 } };
      glClearStencil(0);
      glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
      glEnable(GL_STENCIL_TEST);
      glStencilFunc(GL_ALWAYS, 1, 0xff);
      glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
      draw7(&tri[0][0], 3);
      glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
      glStencilFunc(GL_EQUAL, 1, 0xff);
      draw7(&qg[0][0], 6);
      glStencilFunc(GL_EQUAL, 0, 0xff);
      draw7(&qb[0][0], 6);
      glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
      for (int y = 0; y < h; y++)
         for (int x = 0; x < w; x++) {
            double b[3], want[4];
            int i = inside(&tri[0][0], 7, w, h, x, y, b);
            if (!i)
               continue;
            memcpy(want, i > 0 ? (double[]){ 0, 1, 0, 1 } : (double[]){ 0, 0, 1, 1 }, sizeof(want));
            check(x, y, px + 4 * (y * w + x), want);
         }
   }
   if (glGetError())
      DIE("GL error");
   printf("%s: %s %s %dx%d, %d of %d pixels right\n", bad ? "FAIL" : "ok", mode, arg, w, h, total - bad, total);
   return bad != 0;
}

int
main(int argc, char **argv)
{
   const char *mode = argc > 1 ? argv[1] : "uniform";

   if (!strcmp(mode, "depth") || !strcmp(mode, "cull") || !strcmp(mode, "stencil")) {
      const int a1 = strcmp(mode, "stencil") ? 3 : 2;
      context();
      return zs_main(mode, a1 == 3 && argc > 2 ? argv[2] : "", argc > a1 + 1 ? atoi(argv[a1]) : 64,
                     argc > a1 + 1 ? atoi(argv[a1 + 1]) : 64);
   }
   int tex = strncmp(mode, "uniform", 7) != 0;
   /* uniform: both; uniform-vs / uniform-fs: one stage's only */
   int uvs = strcmp(mode, "uniform-fs") != 0, ufs = strncmp(mode, "uniform-vs", 10) != 0;
   /* uniform-vs4: the transform from one vec4 (a small vertex shader) */
   int vs4 = !strncmp(mode, "uniform-vs4", 11);
   /* uniform-vs4hi: that vec4 after 4 others the shader also reads a little of */
   int hi = !strcmp(mode, "uniform-vs4hi");
   int tw = tex && argc > 3 ? atoi(argv[2]) : 8, th = tex && argc > 3 ? atoi(argv[3]) : 8;
   int a = tex ? 4 : 2;
   int w = argc > a + 1 ? atoi(argv[a]) : 64, h = argc > a + 1 ? atoi(argv[a + 1]) : 64;
   unsigned char *px = malloc(4 * w * h);

   context();
   target(w, h);
   glClearColor(0.25f, 0.5f, 0.75f, 1);
   glClear(GL_COLOR_BUFFER_BIT);

   if (!tex) {
      /* a triangle scaled by 0.75 and moved by (0.1, -0.05) through a
       * matrix, its colours times a tint plus an offset */
      static const float v[3][6] = { { -1, -1, 1, 0, 0, 1 }, { 1, -1, 0, 1, 0, 1 }, { -1, 1, 0, 0, 1, 1 } };
      static const float m[16] = { 0.75f, 0, 0, 0, 0, 0.75f, 0, 0, 0, 0, 1, 0, 0.1f, -0.05f, 0, 1 };
      static const float tint1[4] = { 0.5f, 0.8f, 1.0f, 1.0f }, add1[4] = { 0.1f, 0, 0.05f, 0 };
      static const float one[4] = { 1, 1, 1, 1 }, zero[4] = { 0 };
      const float *tint = ufs ? tint1 : one, *add = ufs ? add1 : zero;
      GLuint p = program(hi ? "uniform vec4 pad[4];\nuniform vec4 s;\nattribute vec4 pos;\nattribute vec4 attr;\n"
                              "varying vec4 v;\nvoid main() { v = attr + pad[0] * 0.0;\n"
                              "gl_Position = vec4(pos.xy * s.x + s.yz, pos.zw); }\n"
                         : vs4 ? "uniform vec4 s;\nattribute vec4 pos;\nattribute vec4 attr;\nvarying vec4 v;\n"
                               "void main() { v = attr; gl_Position = vec4(pos.xy * s.x + s.yz, pos.zw); }\n"
                         : getenv("G9P_TEST_VS") ? getenv("G9P_TEST_VS")
                         : uvs ? "uniform mat4 mvp;\nattribute vec4 pos;\nattribute vec4 attr;\nvarying vec4 v;\n"
                               "void main() { v = attr; gl_Position = mvp * pos; }\n"
                             : "attribute vec4 pos;\nattribute vec4 attr;\nvarying vec4 v;\n"
                               "void main() { v = attr; gl_Position = vec4(pos.xy * 0.75 + vec2(0.1, -0.05), pos.zw); }\n",
                         ufs ? "precision mediump float;\nuniform vec4 tint;\nuniform vec4 add;\nvarying vec4 v;\n"
                               "void main() { gl_FragColor = v * tint + add; }\n"
                             : "precision mediump float;\nvarying vec4 v;\nvoid main() { gl_FragColor = v; }\n");
      if (vs4)
         glUniform4f(glGetUniformLocation(p, "s"), 0.75f, 0.1f, -0.05f, 0);
      else if (uvs)
         glUniformMatrix4fv(glGetUniformLocation(p, "mvp"), 1, GL_FALSE, m);
      if (ufs) {
         glUniform4fv(glGetUniformLocation(p, "tint"), 1, tint);
         glUniform4fv(glGetUniformLocation(p, "add"), 1, add);
      }
      attribs(&v[0][0], 3, 6);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
      double sx[3], sy[3];
      for (int i = 0; i < 3; i++) {
         sx[i] = (v[i][0] * 0.75 + 0.1 + 1) * w / 2;
         sy[i] = (v[i][1] * 0.75 - 0.05 + 1) * h / 2;
      }
      double d = (sy[1] - sy[2]) * (sx[0] - sx[2]) + (sx[2] - sx[1]) * (sy[0] - sy[2]);
      for (int y = 0; y < h; y++)
         for (int x = 0; x < w; x++) {
            double cx = x + 0.5, cy = y + 0.5, b[3], want[4];
            b[0] = ((sy[1] - sy[2]) * (cx - sx[2]) + (sx[2] - sx[1]) * (cy - sy[2])) / d;
            b[1] = ((sy[2] - sy[0]) * (cx - sx[2]) + (sx[0] - sx[2]) * (cy - sy[2])) / d;
            b[2] = 1 - b[0] - b[1];
            double mn = fmin(b[0], fmin(b[1], b[2]));
            if (fabs(mn) < 2.0 / (w < h ? w : h))
               continue;
            for (int k = 0; k < 4; k++)
               want[k] = mn < 0 ? (double[]){ 0.25, 0.5, 0.75, 1 }[k]
                                : (b[0] * v[0][2 + k] + b[1] * v[1][2 + k] + b[2] * v[2][2 + k]) * tint[k] + add[k];
            check(x, y, px + 4 * (y * w + x), want);
         }
   } else {
      /* a quad over the whole target, uv 0..1, the texture nearest */
      static const float v[6][4] = { { -1, -1, 0, 0 }, { 1, -1, 1, 0 }, { 1, 1, 1, 1 },
                                     { -1, -1, 0, 0 }, { 1, 1, 1, 1 }, { -1, 1, 0, 1 } };
      static const float mod[4] = { 0.5f, 1.0f, 0.75f, 1.0f };
      const int texmod = !strcmp(mode, "texmod");
      unsigned char *t = malloc(4 * tw * th);
      GLuint p, to;

      for (int y = 0; y < th; y++)
         for (int x = 0; x < tw; x++)
            texel(x, y, t + 4 * (y * tw + x));
      glGenTextures(1, &to);
      glBindTexture(GL_TEXTURE_2D, to);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, t);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      p = program("attribute vec4 pos;\nattribute vec2 attr;\nvarying vec2 uv;\n"
                  "void main() { uv = attr; gl_Position = pos; }\n",
                  texmod ? "precision mediump float;\nuniform sampler2D t;\nuniform vec4 mod;\nvarying vec2 uv;\n"
                           "void main() { gl_FragColor = texture2D(t, uv) * mod; }\n"
                         : "precision mediump float;\nuniform sampler2D t;\nvarying vec2 uv;\n"
                           "void main() { gl_FragColor = texture2D(t, uv); }\n");
      glUniform1i(glGetUniformLocation(p, "t"), 0);
      if (texmod)
         glUniform4fv(glGetUniformLocation(p, "mod"), 1, mod);
      attribs(&v[0][0], 6, 4);
      glDrawArrays(GL_TRIANGLES, 0, 6);
      glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
      for (int y = 0; y < h; y++)
         for (int x = 0; x < w; x++) {
            double u = (x + 0.5) / w * tw, vv = (y + 0.5) / h * th, want[4];
            unsigned char c[4];
            /* a texel border, or the quad's diagonal */
            if (fabs(u - lround(u)) < 0.05 * tw / w + 0.01 || fabs(vv - lround(vv)) < 0.05 * th / h + 0.01 ||
                abs(x * h - y * w) < 2 * (w > h ? w : h))
               continue;
            texel((int)u, (int)vv, c);
            for (int k = 0; k < 4; k++)
               want[k] = c[k] / 255.0 * (texmod ? mod[k] : 1);
            check(x, y, px + 4 * (y * w + x), want);
         }
   }
   if (glGetError())
      DIE("GL error");
   printf("%s: %s %dx%d%s, %d of %d pixels right\n", bad ? "FAIL" : "ok", mode, w, h,
          tex ? (char[32]){ 0 } : "", total - bad, total);
   if (tex)
      printf("   (texture %dx%d)\n", tw, th);
   return bad != 0;
}
