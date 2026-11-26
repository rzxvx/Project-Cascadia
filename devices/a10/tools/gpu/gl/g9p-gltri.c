// SPDX-License-Identifier: MIT
/*
 * g9p-gltri: OpenGL ES 2.0 through Cascadia's Gallium driver (g9p) without a
 * window -- EGL's surfaceless platform, a framebuffer object W x H, a clear,
 * a triangle with a colour per vertex, glReadPixels -- every pixel compared
 * with the CPU's interpolation (edges left out).
 *
 *   g9p-gltri [W H [frames [mode]]]
 *   mode: arrays (default), elements (u16 index buffer), user8 (u8 indices
 *   from client memory), strip, fan -- the same triangle every way
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DIE(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } while (0)

static const char *vs_src =
   "attribute vec4 pos;\n"
   "attribute vec4 col;\n"
   "varying vec4 v_col;\n"
   "void main() { v_col = col; gl_Position = pos; }\n";
static const char *fs_src =
   "precision mediump float;\n"
   "varying vec4 v_col;\n"
   "void main() { gl_FragColor = v_col; }\n";

/* x, y (clip space), r, g, b, a */
static const float verts[3][6] = {
   { -0.8f, -0.9f, 1, 0, 0, 1 },
   { 0.9f, -0.6f, 0, 1, 0, 1 },
   { -0.3f, 0.85f, 0, 0, 1, 1 },
};
static const float clear[4] = { 0.25f, 0.5f, 0.75f, 1 };

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

int
main(int argc, char **argv)
{
   int w = argc > 2 ? atoi(argv[1]) : 64, h = argc > 2 ? atoi(argv[2]) : 64;
   int frames = argc > 3 ? atoi(argv[3]) : 1;
   const char *mode = argc > 4 ? argv[4] : "arrays";
   static const GLushort idx16[3] = { 0, 1, 2 };
   static const GLubyte idx8[3] = { 0, 1, 2 };
   GLuint ibuf;
   PFNEGLGETPLATFORMDISPLAYEXTPROC gpd =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
   EGLDisplay dpy;
   EGLContext ctx;
   EGLint maj, min, n;
   EGLConfig cfg;
   static const EGLint cattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   static const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                  EGL_NONE };
   GLuint fbo, rb, prog, buf;
   unsigned char *px;
   struct timespec t0, t1;

   if (!gpd || !(dpy = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL)))
      DIE("no surfaceless EGL display");
   if (!eglInitialize(dpy, &maj, &min))
      DIE("eglInitialize");
   if (!eglChooseConfig(dpy, attr, &cfg, 1, &n) || n < 1) {
      /* surfaceless: no config needed */
      cfg = EGL_NO_CONFIG_KHR;
   }
   eglBindAPI(EGL_OPENGL_ES_API);
   if (!(ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cattr)))
      DIE("eglCreateContext: %#x", eglGetError());
   if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
      DIE("eglMakeCurrent: %#x", eglGetError());
   printf("GL_RENDERER: %s\nGL_VERSION: %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glGenRenderbuffers(1, &rb);
   glBindRenderbuffer(GL_RENDERBUFFER, rb);
   glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, w, h);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
   if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
      DIE("framebuffer incomplete");

   prog = glCreateProgram();
   glAttachShader(prog, shader(GL_VERTEX_SHADER, vs_src));
   glAttachShader(prog, shader(GL_FRAGMENT_SHADER, fs_src));
   glBindAttribLocation(prog, 0, "pos");
   glBindAttribLocation(prog, 1, "col");
   glLinkProgram(prog);
   glUseProgram(prog);
   glGenBuffers(1, &buf);
   glBindBuffer(GL_ARRAY_BUFFER, buf);
   glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 24, (void *)0);
   glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 24, (void *)8);
   glEnableVertexAttribArray(0);
   glEnableVertexAttribArray(1);
   glViewport(0, 0, w, h);

   px = malloc(4 * w * h);
   clock_gettime(CLOCK_MONOTONIC, &t0);
   for (int f = 0; f < frames; f++) {
      glClearColor(clear[0], clear[1], clear[2], clear[3]);
      glClear(GL_COLOR_BUFFER_BIT);
      if (!strcmp(mode, "elements")) {
         if (!f) {
            glGenBuffers(1, &ibuf);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibuf);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx16), idx16, GL_STATIC_DRAW);
         }
         glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, 0);
      } else if (!strcmp(mode, "user8")) {
         glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_BYTE, idx8);
      } else if (!strcmp(mode, "strip")) {
         glDrawArrays(GL_TRIANGLE_STRIP, 0, 3);
      } else if (!strcmp(mode, "fan")) {
         glDrawArrays(GL_TRIANGLE_FAN, 0, 3);
      } else {
         glDrawArrays(GL_TRIANGLES, 0, 3);
      }
      glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
   }
   clock_gettime(CLOCK_MONOTONIC, &t1);
   if (glGetError())
      DIE("GL error %#x", glGetError());

   /* the model: GL window coordinates, row 0 at the bottom (glReadPixels') */
   int bad = 0, total = 0, inside = 0;
   double sx[3], sy[3];
   for (int i = 0; i < 3; i++) {
      sx[i] = (verts[i][0] + 1) * w / 2;
      sy[i] = (verts[i][1] + 1) * h / 2;
   }
   double d = (sy[1] - sy[2]) * (sx[0] - sx[2]) + (sx[2] - sx[1]) * (sy[0] - sy[2]);
   for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
         double cx = x + 0.5, cy = y + 0.5, b[3], want[4];
         b[0] = ((sy[1] - sy[2]) * (cx - sx[2]) + (sx[2] - sx[1]) * (cy - sy[2])) / d;
         b[1] = ((sy[2] - sy[0]) * (cx - sx[2]) + (sx[0] - sx[2]) * (cy - sy[2])) / d;
         b[2] = 1 - b[0] - b[1];
         double m = fmin(b[0], fmin(b[1], b[2]));
         if (fabs(m) < 2.0 / (w < h ? w : h))
            continue;                                  /* an edge */
         for (int k = 0; k < 4; k++)
            want[k] = m < 0 ? clear[k] : b[0] * verts[0][2 + k] + b[1] * verts[1][2 + k] + b[2] * verts[2][2 + k];
         inside += m >= 0;
         total++;
         const unsigned char *g = px + 4 * (y * w + x);
         int off = 0;
         for (int k = 0; k < 4; k++)
            if (abs(g[k] - (int)lround(want[k] * 255)) > 3)
               off = 1;
         if (off && bad++ < 5)
            printf("  (%d, %d): got %d %d %d %d want %.0f %.0f %.0f %.0f\n", x, y, g[0], g[1], g[2], g[3],
                   want[0] * 255, want[1] * 255, want[2] * 255, want[3] * 255);
      }
   }
   printf("%s: %s %dx%d, %d of %d pixels right (%d inside the triangle), %.2f ms a frame\n",
          bad ? "FAIL" : "ok", mode, w, h, total - bad, total, inside,
          ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / frames);
   return bad != 0;
}
