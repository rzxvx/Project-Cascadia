// SPDX-License-Identifier: MIT
/*
 * g9p-glwl: an OpenGL ES 2.0 window on Wayland (xdg-shell) -- a turning
 * triangle over a changing background, N frames, the frame rate printed.
 * Through Cascadia's g9p driver on labwc's wl_shm:
 *   GALLIUM_DRIVER=g9p LIBGL_ALWAYS_SOFTWARE=1 g9p-glwl [W H [frames [hold]]]
 * (hold: seconds the last frame stays; frame 0 has the red corner up)
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "xdg-shell-client-protocol.h"

#define DIE(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } while (0)

static struct wl_compositor *compositor;
static struct xdg_wm_base *wm;
static int configured, closed;

static void
global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
   if (!strcmp(iface, "wl_compositor"))
      compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
   else if (!strcmp(iface, "xdg_wm_base"))
      wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
}
static void global_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_l = { global, global_remove };

static void ping(void *d, struct xdg_wm_base *w, uint32_t s) { xdg_wm_base_pong(w, s); }
static const struct xdg_wm_base_listener wm_l = { ping };
static void sconf(void *d, struct xdg_surface *s, uint32_t serial) { xdg_surface_ack_configure(s, serial); configured = 1; }
static const struct xdg_surface_listener s_l = { sconf };
static void tconf(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st) {}
static void tclose(void *d, struct xdg_toplevel *t) { closed = 1; }
static void tbounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) {}
static void tcaps(void *d, struct xdg_toplevel *t, struct wl_array *c) {}
static const struct xdg_toplevel_listener t_l = { tconf, tclose, tbounds, tcaps };

static GLuint
shader(GLenum type, const char *src)
{
   GLuint s = glCreateShader(type);
   GLint ok;
   char log[512];

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
   int w = argc > 2 ? atoi(argv[1]) : 640, h = argc > 2 ? atoi(argv[2]) : 480;
   int frames = argc > 3 ? atoi(argv[3]) : 300;
   struct wl_display *dpy = wl_display_connect(NULL);
   EGLint maj, min, n;
   EGLConfig cfg;
   static const EGLint attr[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                                  EGL_BLUE_SIZE, 8, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE };
   static const EGLint cattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

   if (!dpy)
      DIE("no Wayland display");
   wl_registry_add_listener(wl_display_get_registry(dpy), &reg_l, NULL);
   wl_display_roundtrip(dpy);
   if (!compositor || !wm)
      DIE("no wl_compositor / xdg_wm_base");
   xdg_wm_base_add_listener(wm, &wm_l, NULL);
   struct wl_surface *surf = wl_compositor_create_surface(compositor);
   struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm, surf);
   xdg_surface_add_listener(xs, &s_l, NULL);
   struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
   xdg_toplevel_add_listener(top, &t_l, NULL);
   xdg_toplevel_set_title(top, "g9p-glwl");
   wl_surface_commit(surf);
   while (!configured)
      wl_display_dispatch(dpy);

   EGLDisplay ed = eglGetDisplay((EGLNativeDisplayType)dpy);
   if (!eglInitialize(ed, &maj, &min) || !eglChooseConfig(ed, attr, &cfg, 1, &n) || n < 1)
      DIE("EGL: %#x", eglGetError());
   eglBindAPI(EGL_OPENGL_ES_API);
   struct wl_egl_window *ew = wl_egl_window_create(surf, w, h);
   EGLSurface es = eglCreateWindowSurface(ed, cfg, (EGLNativeWindowType)ew, NULL);
   EGLContext ctx = eglCreateContext(ed, cfg, EGL_NO_CONTEXT, cattr);
   if (!es || !ctx || !eglMakeCurrent(ed, es, es, ctx))
      DIE("EGL surface / context: %#x", eglGetError());
   printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));

   GLuint p = glCreateProgram();
   glAttachShader(p, shader(GL_VERTEX_SHADER,
                            "uniform float a;\nattribute vec2 pos;\nattribute vec3 col;\nvarying vec3 v;\n"
                            "void main() { v = col; float c = cos(a), s = sin(a);\n"
                            "  gl_Position = vec4(c * pos.x - s * pos.y, s * pos.x + c * pos.y, 0.0, 1.0); }\n"));
   glAttachShader(p, shader(GL_FRAGMENT_SHADER,
                            "precision mediump float;\nvarying vec3 v;\nvoid main() { gl_FragColor = vec4(v, 1.0); }\n"));
   glBindAttribLocation(p, 0, "pos");
   glBindAttribLocation(p, 1, "col");
   glLinkProgram(p);
   glUseProgram(p);
   static const float v[] = { 0, 0.8f, 1, 0, 0, -0.7f, -0.5f, 0, 1, 0, 0.7f, -0.5f, 0, 0, 1 };
   GLuint b;
   glGenBuffers(1, &b);
   glBindBuffer(GL_ARRAY_BUFFER, b);
   glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 20, (void *)0);
   glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 20, (void *)8);
   glEnableVertexAttribArray(0);
   glEnableVertexAttribArray(1);
   GLint ua = glGetUniformLocation(p, "a");
   glViewport(0, 0, w, h);

   struct timespec t0, t1;
   clock_gettime(CLOCK_MONOTONIC, &t0);
   int f;
   for (f = 0; f < frames && !closed; f++) {
      float t = f / 60.0f;
      glClearColor(0.2f + 0.2f * sinf(t), 0.25f, 0.35f + 0.2f * cosf(t * 0.7f), 1);
      glClear(GL_COLOR_BUFFER_BIT);
      glUniform1f(ua, t);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      eglSwapBuffers(ed, es);
      wl_display_dispatch_pending(dpy);
   }
   clock_gettime(CLOCK_MONOTONIC, &t1);
   double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
   printf("%d frames %dx%d in %.0f ms: %.1f frames per second\n", f, w, h, ms, f * 1000.0 / ms);
   fflush(stdout);
   if (argc > 4) {
      struct timespec hold = { atoi(argv[4]), 0 };
      nanosleep(&hold, NULL);
   }
   return 0;
}
