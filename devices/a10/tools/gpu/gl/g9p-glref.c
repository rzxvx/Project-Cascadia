// SPDX-License-Identifier: MIT
/*
 * g9p-glref: scenes of real OpenGL ES programs drawn into a framebuffer
 * object and written out, to compare Cascadia's g9p driver with Mesa's
 * softpipe pixel by pixel (GALLIUM_DRIVER=softpipe LIBGL_ALWAYS_SOFTWARE=1
 * for the reference).
 *
 *   g9p-glref gears W H ANGLE OUT.raw [FRAMES]   es2gears's frame (RGBA8, rows bottom up);
 *                                              FRAMES: drawn that often first (angle + 5 each)
 *   g9p-glref plane W H MIN MAG WRAP OUT.raw     a textured floor going into the distance:
 *       MIN nearest | linear | nn | ln | nl | ll (the mipmap filters: min / mip),
 *       MAG nearest | linear, WRAP repeat | mirror | clamp; the texture a 64x64
 *       pattern with glGenerateMipmap's levels
 *   g9p-glref levels W H MIN [TW TH] OUT.raw     the same, each level one colour (the
 *                                                texture TW x TH, default 64 x 64)
 *
 * The gears scene is mesa-demos' es2gears (src/egl/opengles2/es2gears.c)
 * and its matrix helpers (src/util/matrix.c), MIT:
 * Copyright (C) 1999-2001 Brian Paul, (C) 2010 Kristian Hoegsberg,
 * (C) 2010 Chia-I Wu, (C) 2023 Collabora Ltd.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIE(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } while (0)
#define STRIPS_PER_TOOTH 7
#define VERTICES_PER_TOOTH 46
#define GEAR_VERTEX_STRIDE 6

static inline float mat4_get(const float m[4][4], unsigned i, unsigned j) { return m[j][i]; }
static inline void mat4_set(float m[4][4], unsigned i, unsigned j, float v) { m[j][i] = v; }

void
mat4_multiply(float m[4][4], const float n[4][4])
{
   float tmp[4][4];
   int i, j, k;

   for (j = 0; j < 4; j++) {
      for (i = 0; i < 4; i++) {
         float sum = 0.0f;
         for (k = 0; k < 4; k++)
            sum += mat4_get(m, i, k) * mat4_get(n, k, j);
         mat4_set(tmp, i, j, sum);
      }
   }
   memcpy(m, &tmp, sizeof tmp);
}

void
mat4_scale(float m[4][4], float x, float y, float z)
{
   float s[4][4] = {
      { x, 0, 0, 0 },
      { 0, y, 0, 0 },
      { 0, 0, z, 0 },
      { 0, 0, 0, 1 }
   };
   mat4_multiply(m, s);
}

void
mat4_rotate(float m[4][4], float angle, float x, float y, float z)
{
   double s, c;
#if HAVE_SINCOS
   sincos(angle, &s, &c);
#else
   s = sin(angle);
   c = cos(angle);
#endif
   float r[4][4] = {
      { x * x * (1 - c) + c,     y * x * (1 - c) + z * s, x * z * (1 - c) - y * s, 0 },
      { x * y * (1 - c) - z * s, y * y * (1 - c) + c,     y * z * (1 - c) + x * s, 0 },
      { x * z * (1 - c) + y * s, y * z * (1 - c) - x * s, z * z * (1 - c) + c,     0 },
      { 0, 0, 0, 1 }
   };

   mat4_multiply(m, r);
}

void
mat4_translate(float m[4][4], float x, float y, float z)
{
   float t[4][4] = {
      { 1, 0, 0, 0 },
      { 0, 1, 0, 0 },
      { 0, 0, 1, 0 },
      { x, y, z, 1 }
   };

   mat4_multiply(m, t);
}

void
mat4_identity(float m[4][4])
{
   float t[4][4] = {
      { 1.0, 0.0, 0.0, 0.0 },
      { 0.0, 1.0, 0.0, 0.0 },
      { 0.0, 0.0, 1.0, 0.0 },
      { 0.0, 0.0, 0.0, 1.0 }
   };

   memcpy(m, t, sizeof(t));
}

void
mat4_transpose(float m[4][4])
{
   float t[4][4];
   for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j)
         t[i][j] = m[j][i];

   memcpy(m, t, sizeof(t));
}

void
mat4_invert(float m[4][4])
{
   float t[4][4];
   mat4_identity(t);

   // Extract and invert the translation part 't'. The inverse of a
   // translation matrix can be calculated by negating the translation
   // coordinates.
   for (int i = 0; i < 3; ++i)
      mat4_set(t, i, 3, -mat4_get(m, i, 3));

   // Invert the rotation part 'r'. The inverse of a rotation matrix is
   // equal to its transpose.
   for (int i = 0; i < 3; ++i)
      mat4_set(m, i, 3, 0.0f);
   mat4_transpose(m);

   // inv(m) = inv(r) * inv(t)
   mat4_multiply(m, t);
}

void
mat4_frustum_gl(float m[4][4], float l, float r, float b, float t, float n, float f)
{
   mat4_identity(m);

   float deltaX = r - l;
   float deltaY = t - b;
   float deltaZ = f - n;

   mat4_set(m, 0, 0, (2 * n) / deltaX);
   mat4_set(m, 1, 1, (2 * n) / deltaY);
   mat4_set(m, 0, 2, (r + l) / deltaX);
   mat4_set(m, 1, 2, (t + b) / deltaY);
   mat4_set(m, 2, 2, -(f + n) / deltaZ);
   mat4_set(m, 3, 2, -1.0f);
   mat4_set(m, 2, 3, -(2 * f * n) / deltaZ);
   mat4_set(m, 3, 3, 0.0f);
}


typedef GLfloat GearVertex[GEAR_VERTEX_STRIDE];

/**
 * Struct representing a gear.
 */
struct gear {
   /** The array of vertices comprising the gear */
   GearVertex *vertices;
   /** The number of vertices comprising the gear */
   int nvertices;
   /** The Vertex Buffer Object holding the vertices in the graphics card */
   GLuint vbo;
};

/** The view rotation [x, y, z] */
static GLfloat view_rot[3] = { 20.0, 30.0, 0.0 };

static GLuint ModelViewProjectionMatrix_location, NormalMatrix_location, LightSourcePosition_location,
   MaterialColor_location;
static GLfloat ProjectionMatrix[4][4];
static const GLfloat LightSourcePosition[4] = { 5.0, 5.0, 10.0, 1.0 };

/**
 * Fills a gear vertex.
 *
 * @param v the vertex to fill
 * @param x the x coordinate
 * @param y the y coordinate
 * @param z the z coortinate
 * @param n pointer to the normal table
 *
 * @return the operation error code
 */
static GearVertex *
vert(GearVertex *v, GLfloat x, GLfloat y, GLfloat z, GLfloat n[3])
{
   v[0][0] = x;
   v[0][1] = y;
   v[0][2] = z;
   v[0][3] = n[0];
   v[0][4] = n[1];
   v[0][5] = n[2];

   return v + 1;
}

/**
 *  Create a gear wheel.
 *
 *  @param inner_radius radius of hole at center
 *  @param outer_radius radius at center of teeth
 *  @param width width of gear
 *  @param teeth number of teeth
 *  @param tooth_depth depth of tooth
 *
 *  @return pointer to the constructed struct gear
 */
static struct gear *
create_gear(GLfloat inner_radius, GLfloat outer_radius, GLfloat width,
      GLint teeth, GLfloat tooth_depth)
{
   GLfloat r0, r1, r2;
   GLfloat da;
   GearVertex *v;
   struct gear *gear;
   double s[5], c[5];
   GLfloat normal[3];
   int cur_strip_start = 0;
   int i;

   /* Allocate memory for the gear */
   gear = malloc(sizeof *gear);
   if (gear == NULL)
      return NULL;

   /* Calculate the radii used in the gear */
   r0 = inner_radius;
   r1 = outer_radius - tooth_depth / 2.0;
   r2 = outer_radius + tooth_depth / 2.0;

   da = 2.0 * M_PI / teeth / 4.0;

   /* the first tooth doesn't need the first strip-restart sequence */
   assert(teeth > 0);
   gear->nvertices = VERTICES_PER_TOOTH +
                     (VERTICES_PER_TOOTH + 2) * (teeth - 1);

   /* Allocate memory for the vertices */
   gear->vertices = calloc(gear->nvertices, sizeof(*gear->vertices));
   v = gear->vertices;

   for (i = 0; i < teeth; i++) {
      /* Calculate needed sin/cos for varius angles */
#if HAVE_SINCOS
      sincos(i * 2.0 * M_PI / teeth, &s[0], &c[0]);
      sincos(i * 2.0 * M_PI / teeth + da, &s[1], &c[1]);
      sincos(i * 2.0 * M_PI / teeth + da * 2, &s[2], &c[2]);
      sincos(i * 2.0 * M_PI / teeth + da * 3, &s[3], &c[3]);
      sincos(i * 2.0 * M_PI / teeth + da * 4, &s[4], &c[4]);
#else
     s[0] = sin(i * 2.0 * M_PI / teeth);
     s[1] = sin(i * 2.0 * M_PI / teeth + da);
     s[2] = sin(i * 2.0 * M_PI / teeth + da * 2);
     s[3] = sin(i * 2.0 * M_PI / teeth + da * 3);
     s[4] = sin(i * 2.0 * M_PI / teeth + da * 4);

     c[0] = cos(i * 2.0 * M_PI / teeth);
     c[1] = cos(i * 2.0 * M_PI / teeth + da);
     c[2] = cos(i * 2.0 * M_PI / teeth + da * 2);
     c[3] = cos(i * 2.0 * M_PI / teeth + da * 3);
     c[4] = cos(i * 2.0 * M_PI / teeth + da * 4);
#endif

      /* A set of macros for making the creation of the gears easier */
#define  GEAR_POINT(r, da) { (r) * c[(da)], (r) * s[(da)] }
#define  SET_NORMAL(x, y, z) do { \
   normal[0] = (x); normal[1] = (y); normal[2] = (z); \
} while(0)

#define  GEAR_VERT(v, point, sign) vert((v), p[(point)].x, p[(point)].y, (sign) * width * 0.5, normal)

#define START_STRIP do { \
   cur_strip_start = (v - gear->vertices); \
   if (cur_strip_start) \
      v += 2; \
} while(0);

/* emit prev last vertex
   emit first vertex */
#define END_STRIP do { \
   if (cur_strip_start) { \
      memcpy(gear->vertices + cur_strip_start, \
             gear->vertices + (cur_strip_start - 1), sizeof(GearVertex)); \
      memcpy(gear->vertices + cur_strip_start + 1, \
             gear->vertices + (cur_strip_start + 2), sizeof(GearVertex)); \
   } \
} while (0)

#define QUAD_WITH_NORMAL(p1, p2) do { \
   SET_NORMAL((p[(p1)].y - p[(p2)].y), -(p[(p1)].x - p[(p2)].x), 0); \
   v = GEAR_VERT(v, (p1), -1); \
   v = GEAR_VERT(v, (p1), 1); \
   v = GEAR_VERT(v, (p2), -1); \
   v = GEAR_VERT(v, (p2), 1); \
} while(0)

      struct point {
         GLfloat x;
         GLfloat y;
      };

      /* Create the 7 points (only x,y coords) used to draw a tooth */
      struct point p[7] = {
         GEAR_POINT(r2, 1), // 0
         GEAR_POINT(r2, 2), // 1
         GEAR_POINT(r1, 0), // 2
         GEAR_POINT(r1, 3), // 3
         GEAR_POINT(r0, 0), // 4
         GEAR_POINT(r1, 4), // 5
         GEAR_POINT(r0, 4), // 6
      };

      /* Front face */
      START_STRIP;
      SET_NORMAL(0, 0, 1.0);
      v = GEAR_VERT(v, 0, +1);
      v = GEAR_VERT(v, 1, +1);
      v = GEAR_VERT(v, 2, +1);
      v = GEAR_VERT(v, 3, +1);
      v = GEAR_VERT(v, 4, +1);
      v = GEAR_VERT(v, 5, +1);
      v = GEAR_VERT(v, 6, +1);
      END_STRIP;

      /* Back face */
      START_STRIP;
      SET_NORMAL(0, 0, -1.0);
      v = GEAR_VERT(v, 0, -1);
      v = GEAR_VERT(v, 1, -1);
      v = GEAR_VERT(v, 2, -1);
      v = GEAR_VERT(v, 3, -1);
      v = GEAR_VERT(v, 4, -1);
      v = GEAR_VERT(v, 5, -1);
      v = GEAR_VERT(v, 6, -1);
      END_STRIP;

      /* Outer face */
      START_STRIP;
      QUAD_WITH_NORMAL(0, 2);
      END_STRIP;

      START_STRIP;
      QUAD_WITH_NORMAL(1, 0);
      END_STRIP;

      START_STRIP;
      QUAD_WITH_NORMAL(3, 1);
      END_STRIP;

      START_STRIP;
      QUAD_WITH_NORMAL(5, 3);
      END_STRIP;

      /* Inner face */
      START_STRIP;
      SET_NORMAL(-c[0], -s[0], 0);
      v = GEAR_VERT(v, 4, -1);
      v = GEAR_VERT(v, 4, 1);
      SET_NORMAL(-c[4], -s[4], 0);
      v = GEAR_VERT(v, 6, -1);
      v = GEAR_VERT(v, 6, 1);
      END_STRIP;
   }

   assert(gear->nvertices == (v - gear->vertices));

   /* Store the vertices in a vertex buffer object (VBO) */
   glGenBuffers(1, &gear->vbo);
   glBindBuffer(GL_ARRAY_BUFFER, gear->vbo);
   glBufferData(GL_ARRAY_BUFFER, gear->nvertices * sizeof(GearVertex),
         gear->vertices, GL_STATIC_DRAW);

   return gear;
}

/**
 * Draws a gear.
 *
 * @param gear the gear to draw
 * @param transform the current transformation matrix
 * @param x the x position to draw the gear at
 * @param y the y position to draw the gear at
 * @param angle the rotation angle of the gear
 * @param color the color of the gear
 */
static void
draw_gear(struct gear *gear, const float transform[4][4],
      GLfloat x, GLfloat y, GLfloat angle, const GLfloat color[4])
{
   float model_view[4][4];
   float normal_matrix[4][4];
   float model_view_projection[4][4];

   /* Translate and rotate the gear */
   memcpy(model_view, transform, sizeof (model_view));
   mat4_translate(model_view, x, y, 0);
   mat4_rotate(model_view, 2 * M_PI * angle / 360.0, 0, 0, 1);

   /* Create and set the ModelViewProjectionMatrix */
   memcpy(model_view_projection, ProjectionMatrix, sizeof(model_view_projection));
   mat4_multiply(model_view_projection, model_view);

   glUniformMatrix4fv(ModelViewProjectionMatrix_location, 1, GL_FALSE,
                      (const GLfloat *)model_view_projection);

   /*
    * Create and set the NormalMatrix. It's the inverse transpose of the
    * ModelView matrix.
    */
   memcpy(normal_matrix, model_view, sizeof (normal_matrix));
   mat4_invert(normal_matrix);
   mat4_transpose(normal_matrix);
   glUniformMatrix4fv(NormalMatrix_location, 1, GL_FALSE,
                      (const GLfloat *)normal_matrix);

   /* Set the gear color */
   glUniform4fv(MaterialColor_location, 1, color);

   /* Set the vertex buffer object to use */
   glBindBuffer(GL_ARRAY_BUFFER, gear->vbo);

   /* Set up the position of the attributes in the vertex buffer object */
   glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
         6 * sizeof(GLfloat), NULL);
   glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
         6 * sizeof(GLfloat), (GLfloat *) (sizeof(GLfloat) * 3));

   /* Enable the attributes */
   glEnableVertexAttribArray(0);
   glEnableVertexAttribArray(1);

   /* Draw the triangle strips that comprise the gear */
   glDrawArrays(GL_TRIANGLE_STRIP, 0, gear->nvertices);

   /* Disable the attributes */
   glDisableVertexAttribArray(1);
   glDisableVertexAttribArray(0);
}

static const char vertex_shader[] =
"attribute vec3 position;\n"
"attribute vec3 normal;\n"
"\n"
"uniform mat4 ModelViewProjectionMatrix;\n"
"uniform mat4 NormalMatrix;\n"
"uniform vec4 LightSourcePosition;\n"
"uniform vec4 MaterialColor;\n"
"\n"
"varying vec4 Color;\n"
"\n"
"void main(void)\n"
"{\n"
"    // Transform the normal to eye coordinates\n"
"    vec3 N = normalize(vec3(NormalMatrix * vec4(normal, 1.0)));\n"
"\n"
"    // The LightSourcePosition is actually its direction for directional light\n"
"    vec3 L = normalize(LightSourcePosition.xyz);\n"
"\n"
"    float diffuse = max(dot(N, L), 0.0);\n"
"    float ambient = 0.2;\n"
"\n"
"    // Multiply the diffuse value by the vertex color (which is fixed in this case)\n"
"    // to get the actual color that we will use to draw this vertex with\n"
"    Color = vec4((ambient + diffuse) * MaterialColor.xyz, MaterialColor.a);\n"
"\n"
"    // Transform the position to clip coordinates\n"
"    gl_Position = ModelViewProjectionMatrix * vec4(position, 1.0);\n"
"}";

static const char fragment_shader[] =
"precision mediump float;\n"
"varying vec4 Color;\n"
"\n"
"void main(void)\n"
"{\n"
"    gl_FragColor = Color;\n"
"}";


static GLenum
min_filter(const char *m)
{
   if (!strcmp(m, "linear")) return GL_LINEAR;
   if (!strcmp(m, "nn")) return GL_NEAREST_MIPMAP_NEAREST;
   if (!strcmp(m, "ln")) return GL_LINEAR_MIPMAP_NEAREST;
   if (!strcmp(m, "nl")) return GL_NEAREST_MIPMAP_LINEAR;
   if (!strcmp(m, "ll")) return GL_LINEAR_MIPMAP_LINEAR;
   return GL_NEAREST;
}

static void
egl_fbo(int w, int h)
{
   PFNEGLGETPLATFORMDISPLAYEXTPROC gpd =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
   static const EGLint cattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLDisplay dpy;
   EGLint maj, min;
   if (!gpd || !(dpy = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL)) || !eglInitialize(dpy, &maj, &min))
      DIE("no surfaceless EGL display");
   eglBindAPI(EGL_OPENGL_ES_API);
   EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, cattr);
   if (!ctx || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
      DIE("context");
   GLuint fbo, rb;
   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glGenRenderbuffers(1, &rb);
   glBindRenderbuffer(GL_RENDERBUFFER, rb);
   glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, w, h);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
   glViewport(0, 0, w, h);
}

static void
write_out(const char *path, int w, int h)
{
   unsigned char *px = malloc(4 * w * h);
   glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
   FILE *o = fopen(path, "wb");
   if (!o || fwrite(px, 4, w * h, o) != (size_t)(w * h))
      DIE("%s", path);
   fclose(o);
   printf("%s: %dx%d written (GL error %#x)\n", path, w, h, glGetError());
}

/* a floor: z from -1.5 (near) to -40, x -4..4 at y -1, uv 0..8 x 0..40,
 * under a 60-degree perspective */
static int
plane(int argc, char **argv)
{
   const int w = atoi(argv[2]), h = atoi(argv[3]);
   const int levels = !strcmp(argv[1], "levels");
   const char *minf = argv[4], *magf = levels ? "nearest" : argv[5], *wrap = levels ? "repeat" : argv[6];
   const char *out = argv[argc - 1];
   egl_fbo(w, h);
   glClearColor(0.25f, 0.5f, 0.75f, 1);
   glClear(GL_COLOR_BUFFER_BIT);
   GLuint t;
   glGenTextures(1, &t);
   glBindTexture(GL_TEXTURE_2D, t);
   if (levels) {
      const int tw = argc > 7 ? atoi(argv[5]) : 64, th = argc > 7 ? atoi(argv[6]) : 64;
      static const unsigned char c[7][4] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 },
                                             { 255, 255, 0, 255 }, { 255, 0, 255, 255 }, { 0, 255, 255, 255 },
                                             { 255, 255, 255, 255 } };
      unsigned char *b = malloc(4 * tw * th);
      for (int l = 0; (tw >> l) || (th >> l); l++) {
         const int lw = tw >> l ? tw >> l : 1, lh = th >> l ? th >> l : 1;
         for (int i = 0; i < lw * lh; i++)
            memcpy(b + 4 * i, c[l % 7], 4);
         glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA, lw, lh, 0, GL_RGBA, GL_UNSIGNED_BYTE, b);
      }
   } else {
      unsigned char *b = malloc(4 * 64 * 64);
      for (int y = 0; y < 64; y++)
         for (int x = 0; x < 64; x++) {
            unsigned char *p = b + 4 * (y * 64 + x);
            p[0] = ((x / 8 + y / 8) & 1) ? 230 : 30;
            p[1] = x * 4;
            p[2] = y * 4;
            p[3] = 255;
         }
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, b);
      glGenerateMipmap(GL_TEXTURE_2D);
   }
   const GLenum wm = !strcmp(wrap, "mirror") ? GL_MIRRORED_REPEAT : !strcmp(wrap, "clamp") ? GL_CLAMP_TO_EDGE : GL_REPEAT;
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter(minf));
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, !strcmp(magf, "linear") ? GL_LINEAR : GL_NEAREST);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wm);
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wm);
   const char *vs = "uniform mat4 mvp;\nattribute vec4 pos;\nattribute vec2 uv;\nvarying vec2 v;\n"
                    "void main() { v = uv; gl_Position = mvp * pos; }\n";
   const char *fs = "precision mediump float;\nuniform sampler2D t;\nvarying vec2 v;\n"
                    "void main() { gl_FragColor = texture2D(t, v); }\n";
   GLuint sv = glCreateShader(GL_VERTEX_SHADER), sf = glCreateShader(GL_FRAGMENT_SHADER), pr = glCreateProgram();
   glShaderSource(sv, 1, &vs, NULL);
   glCompileShader(sv);
   glShaderSource(sf, 1, &fs, NULL);
   glCompileShader(sf);
   glAttachShader(pr, sv);
   glAttachShader(pr, sf);
   glBindAttribLocation(pr, 0, "pos");
   glBindAttribLocation(pr, 1, "uv");
   glLinkProgram(pr);
   glUseProgram(pr);
   float m[4][4];
   mat4_frustum_gl(m, -1.0, 1.0, -(float)h / w, (float)h / w, 1.0, 50.0);
   glUniformMatrix4fv(glGetUniformLocation(pr, "mvp"), 1, GL_FALSE, &m[0][0]);
   glUniform1i(glGetUniformLocation(pr, "t"), 0);
   static const float q[6][6] = {
      { -4, -1, -1.5f, 1, -1.5f, 0 }, { 4, -1, -1.5f, 1, 2.5f, 0 }, { 4, -1, -40, 1, 2.5f, 40 },
      { -4, -1, -1.5f, 1, -1.5f, 0 }, { 4, -1, -40, 1, 2.5f, 40 }, { -4, -1, -40, 1, -1.5f, 40 } };
   GLuint b;
   glGenBuffers(1, &b);
   glBindBuffer(GL_ARRAY_BUFFER, b);
   glBufferData(GL_ARRAY_BUFFER, sizeof(q), q, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 24, (void *)0);
   glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 24, (void *)16);
   glEnableVertexAttribArray(0);
   glEnableVertexAttribArray(1);
   glDrawArrays(GL_TRIANGLES, 0, 6);
   write_out(out, w, h);
   return 0;
}

int
main(int argc, char **argv)
{
   if (argc >= 6 && !strcmp(argv[1], "levels"))
      return plane(argc, argv);
   if (argc >= 8 && !strcmp(argv[1], "plane"))
      return plane(argc, argv);
   if (argc < 6 || strcmp(argv[1], "gears"))
      DIE("usage: g9p-glref gears W H ANGLE OUT.raw");
   int w = atoi(argv[2]), h = atoi(argv[3]);
   float angle = atof(argv[4]);
   PFNEGLGETPLATFORMDISPLAYEXTPROC gpd =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
   static const EGLint cattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLDisplay dpy;
   EGLint maj, min;
   if (!gpd || !(dpy = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL)) || !eglInitialize(dpy, &maj, &min))
      DIE("no surfaceless EGL display");
   eglBindAPI(EGL_OPENGL_ES_API);
   EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, cattr);
   if (!ctx || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
      DIE("context");
   GLuint fbo, rb[2];
   glGenFramebuffers(1, &fbo);
   glBindFramebuffer(GL_FRAMEBUFFER, fbo);
   glGenRenderbuffers(2, rb);
   glBindRenderbuffer(GL_RENDERBUFFER, rb[0]);
   glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, w, h);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb[0]);
   glBindRenderbuffer(GL_RENDERBUFFER, rb[1]);
   glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
   glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb[1]);

   /* es2gears's gears_init and gears_reshape */
   GLuint v, f, program;
   const char *p;
   char msg[512];
   glEnable(GL_CULL_FACE);
   glEnable(GL_DEPTH_TEST);
   p = vertex_shader;
   v = glCreateShader(GL_VERTEX_SHADER);
   glShaderSource(v, 1, &p, NULL);
   glCompileShader(v);
   glGetShaderInfoLog(v, sizeof msg, NULL, msg);
   p = fragment_shader;
   f = glCreateShader(GL_FRAGMENT_SHADER);
   glShaderSource(f, 1, &p, NULL);
   glCompileShader(f);
   program = glCreateProgram();
   glAttachShader(program, v);
   glAttachShader(program, f);
   glBindAttribLocation(program, 0, "position");
   glBindAttribLocation(program, 1, "normal");
   glLinkProgram(program);
   glUseProgram(program);
   ModelViewProjectionMatrix_location = glGetUniformLocation(program, "ModelViewProjectionMatrix");
   NormalMatrix_location = glGetUniformLocation(program, "NormalMatrix");
   LightSourcePosition_location = glGetUniformLocation(program, "LightSourcePosition");
   MaterialColor_location = glGetUniformLocation(program, "MaterialColor");
   glUniform4fv(LightSourcePosition_location, 1, LightSourcePosition);
   struct gear *gear1 = create_gear(1.0, 4.0, 1.0, 20, 0.7);
   struct gear *gear2 = create_gear(0.5, 2.0, 2.0, 10, 0.7);
   struct gear *gear3 = create_gear(1.3, 2.0, 0.5, 10, 0.7);
   GLfloat hh = (GLfloat)h / (GLfloat)w;
   mat4_frustum_gl(ProjectionMatrix, -1.0, 1.0, -hh, hh, 5.0, 60.0);
   glViewport(0, 0, w, h);

   const int frames = argc > 6 ? atoi(argv[6]) : 1;
   for (int fr = 0; fr < frames; fr++, angle += 5) {
   /* gears_draw */
   static const GLfloat red[4] = { 0.8, 0.1, 0.0, 1.0 }, green[4] = { 0.0, 0.8, 0.2, 1.0 },
                        blue[4] = { 0.2, 0.2, 1.0, 1.0 };
   static const GLfloat view_rot[3] = { 20.0, 30.0, 0.0 };
   float transform[4][4];
   mat4_identity(transform);
   glClearColor(0.0, 0.0, 0.0, 1.0);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
   mat4_translate(transform, 0, 0, -40);
   mat4_rotate(transform, 2 * M_PI * view_rot[0] / 360.0, 1, 0, 0);
   mat4_rotate(transform, 2 * M_PI * view_rot[1] / 360.0, 0, 1, 0);
   mat4_rotate(transform, 2 * M_PI * view_rot[2] / 360.0, 0, 0, 1);
   draw_gear(gear1, transform, -3.0, -2.0, angle, red);
   draw_gear(gear2, transform, 3.1, -2.0, -2 * angle - 9.0, green);
   draw_gear(gear3, transform, -3.1, 4.2, -2 * angle - 25.0, blue);
   if (fr + 1 < frames)
      glFlush();
   }
   angle -= 5;

   unsigned char *px = malloc(4 * w * h);
   glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
   FILE *o = fopen(argv[5], "wb");
   if (!o || fwrite(px, 4, w * h, o) != (size_t)(w * h))
      DIE("%s", argv[5]);
   fclose(o);
   printf("%s: %dx%d written (GL error %#x)\n", argv[5], w, h, glGetError());
   return 0;
}
