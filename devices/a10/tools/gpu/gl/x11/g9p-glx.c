// SPDX-License-Identifier: MIT
/*
 * g9p-glx: desktop OpenGL through GLX on X11 (Xwayland on the iPad's
 * desktop), a window W x H, one thing drawn the old fixed-function way,
 * the middle pixel read back and printed, the frame shown -- what is drawn
 * apart from how it reaches the screen.
 *
 *   g9p-glx MODE [seconds]
 *   MODE: clear (red), tri (glBegin, a colour per vertex), flat (GL_FLAT),
 *   lit (one light, a material), list (the triangle in a display list),
 *   and their combinations (litflat, litflatlist, ...); "persp" in the
 *   name: a perspective projection, the picture tilted (w 2..4),
 *   flatstrip / flatfan (GL_FLAT, a colour per vertex), with a "1" suffix
 *   the first vertex's colour (glProvokingVertex)
 *   G9P_GLX_DUMP=FILE: the first frame's RGBA rows (bottom first) there
 */
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DIE(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } while (0)

static const float cols[6][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 1, 1, 0 }, { 0, 1, 1 }, { 1, 0, 1 } };

static void
many(GLenum prim)
{
   glBegin(prim);
   if (prim == GL_TRIANGLE_FAN) {
      static const float v[6][2] = { { 0, 0 }, { 0.9f, 0 }, { 0.5f, 0.8f }, { -0.5f, 0.8f }, { -0.9f, 0 },
                                     { -0.4f, -0.8f } };
      for (int i = 0; i < 6; i++) {
         glColor3fv(cols[i]);
         glVertex2fv(v[i]);
      }
   } else {
      for (int i = 0; i < 6; i++) {
         glColor3fv(cols[i]);
         glVertex2f(-0.9f + 0.36f * i, i & 1 ? 0.7f : -0.7f);
      }
   }
   glEnd();
}

static int quad;

static void
tri(void)
{
   if (quad) {
      /* a quad strip of two quads, a normal per quad as glxgears sets them */
      glBegin(GL_QUAD_STRIP);
      for (int i = 0; i < 6; i++) {
         if (!(i & 1))
            glNormal3f(i == 2 ? 0.6f : 0, 0, i == 2 ? 0.8f : 1);
         glColor3fv(cols[i]);
         glVertex2f(-0.8f + 0.32f * (i / 2) * 2.5f, i & 1 ? 0.7f : -0.7f);
      }
      glEnd();
      return;
   }
   glBegin(GL_TRIANGLES);
   glColor3f(1, 0, 0);
   glNormal3f(0, 0, 1);
   glVertex3f(-0.8f, -0.8f, 0);
   glColor3f(0, 1, 0);
   glVertex3f(0.8f, -0.8f, 0);
   glColor3f(0, 0, 1);
   glVertex3f(0, 0.8f, 0);
   glEnd();
}

int
main(int argc, char **argv)
{
   const char *mode = argc > 1 ? argv[1] : "clear";
   const double secs = argc > 2 ? atof(argv[2]) : 3;
   const int w = 256, h = 256;
   static int attr[] = { GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 1, GLX_GREEN_SIZE, 1, GLX_BLUE_SIZE, 1,
                         GLX_DEPTH_SIZE, 1, None };
   Display *dpy = XOpenDisplay(NULL);
   XVisualInfo *vi;
   XSetWindowAttributes swa = { 0 };
   Window win;
   GLXContext ctx;
   GLuint list = 0;
   unsigned char px[4];
   struct timespec t0, t;
   int frames = 0;

   if (!dpy)
      DIE("no X display");
   if (!(vi = glXChooseVisual(dpy, DefaultScreen(dpy), attr)))
      DIE("no visual");
   swa.colormap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
   swa.event_mask = StructureNotifyMask;
   win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 0, 0, w, h, 0, vi->depth, InputOutput, vi->visual,
                       CWColormap | CWEventMask, &swa);
   XStoreName(dpy, win, "g9p-glx");
   XMapWindow(dpy, win);
   if (!(ctx = glXCreateContext(dpy, vi, NULL, True)))
      DIE("glXCreateContext");
   glXMakeCurrent(dpy, win, ctx);
   printf("GL_RENDERER: %s\nGL_VERSION: %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
   glViewport(0, 0, w, h);
   if (strstr(mode, "persp")) {
      /* w from 2 to 4 across the picture: x tilted away */
      glMatrixMode(GL_PROJECTION);
      glFrustum(-0.5, 0.5, -0.5, 0.5, 1, 10);
      glMatrixMode(GL_MODELVIEW);
      glTranslatef(0, 0, -3);
      glRotatef(50, 0, 1, 0);
   }
   quad = strstr(mode, "quad") != NULL;
   if (strstr(mode, "flat"))
      glShadeModel(GL_FLAT);
   if (mode[strlen(mode) - 1] == '1')
      glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
   if (strstr(mode, "lit")) {
      static const GLfloat pos[4] = { 0, 0, 1, 0 }, red[4] = { 1, 0.2f, 0.1f, 1 };
      glEnable(GL_LIGHTING);
      glEnable(GL_LIGHT0);
      glLightfv(GL_LIGHT0, GL_POSITION, pos);
      glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, red);
   }
   if (strstr(mode, "list")) {
      list = glGenLists(1);
      glNewList(list, GL_COMPILE);
      tri();
      glEndList();
   }
   clock_gettime(CLOCK_MONOTONIC, &t0);
   do {
      if (!strcmp(mode, "clear"))
         glClearColor(1, 0, 0, 1);
      else
         glClearColor(0.1f, 0.1f, 0.3f, 1);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
      if (!strncmp(mode, "flatid", 6)) {
         /* a strip of 6: vertex i's colour R = (i + 1) / 8, G = 1 - R */
         glBegin(GL_TRIANGLE_STRIP);
         for (int i = 0; i < 6; i++) {
            glColor3f((i + 1) / 8.0f, 1 - (i + 1) / 8.0f, 0.25f);
            glVertex2f(-0.9f + 0.36f * i, i & 1 ? 0.7f : -0.7f);
         }
         glEnd();
         if (!frames)
            for (int t = 0; t < 4; t++) {
               unsigned char q[4];
               glReadPixels((int)((1 - 0.9f + 0.36f * (t + 1)) / 2 * w), h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q);
               printf("  triangle %d: R*8/255 %.2f G*8/255 %.2f B %d\n", t, q[0] * 8 / 255.0, q[1] * 8 / 255.0, q[2]);
            }
      } else if (!strncmp(mode, "flatstrip", 9))
         many(GL_TRIANGLE_STRIP);
      else if (!strncmp(mode, "flatfan", 7))
         many(GL_TRIANGLE_FAN);
      else if (list)
         glCallList(list);
      else if (strcmp(mode, "clear"))
         tri();
      if (!frames) {
         glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
         if (getenv("G9P_GLX_DUMP")) {
            static unsigned char all[256 * 256 * 4];
            FILE *f = fopen(getenv("G9P_GLX_DUMP"), "wb");
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, all);
            if (f) {
               fwrite(all, 1, sizeof(all), f);
               fclose(f);
            }
         }
      }
      glXSwapBuffers(dpy, win);
      frames++;
      clock_gettime(CLOCK_MONOTONIC, &t);
   } while ((t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9 < secs);
   printf("%s: middle pixel %d %d %d %d, %d frames, GL error %#x\n", mode, px[0], px[1], px[2], px[3], frames,
          glGetError());
   return 0;
}
