/* glbuffer -- vertex buffers written by glBufferSubData while draws read
 * them (docs/research/p105-mesa.md, M46: the driver writes a vertex
 * buffer's GPU copy directly when no render reads it, its CPU copy then
 * behind until something reads that).  Desktop OpenGL through EGL, as
 * PPSSPP uploads its vertices; every case draws quads whose colours come
 * from a buffer and checks the pixels (or the bytes read back):
 *
 *   direct      a draw, glFinish, new colours, a draw: the new ones (two
 *               strides: the CPU repacks the vertices, reading the buffer)
 *   readback    glGetBufferSubData after such a write: the new bytes
 *   busy        a draw gathered, new colours before it is rendered, a
 *               draw into another target: each its own colours
 *   fetched     the same with x y and colour interleaved, one stride: the
 *               GPU fetches the vertices from the buffer itself
 *   lines       lines drawn from the buffer (the draw module reads it)
 *   map         glMapBufferRange of part of it, written, a draw
 *   indexed     glDrawElements from an index buffer, at 0, 12 and 26 bytes
 *               (an odd first index), the indices written again, read back
 *   invalidate  PPSSPP's push buffers (the driver maps the GPU copy itself,
 *               a fresh one while a render reads the last): a vertex and an
 *               index buffer of 64 and 8 KiB mapped whole with
 *               GL_MAP_INVALIDATE_BUFFER_BIT, written, drawn from, mapped
 *               again while those draws are gathered, running or done; at
 *               offsets (an odd first index); twice in one render; read
 *               back; drawn as lines (the draw module reads them); a
 *               buffer under 4 KiB; glBufferData of all of a busy one
 *
 *   sgx-gl glbuffer [CASE]      (a name's start; all by default) */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 32
#define H 32

static int cases, failed;
static const char *only;
static GLuint fbo[2], rt[2], vbo, prog;

static const char *vs =
	"#version 120\n"
	"attribute vec2 p;\n"
	"attribute vec4 c;\n"
	"varying vec4 vc;\n"
	"void main() { vc = c; gl_Position = vec4(p, 0.0, 1.0); }\n";
static const char *fs =
	"#version 120\n"
	"varying vec4 vc;\n"
	"void main() { gl_FragColor = vc; }\n";

/* the buffer: 6 positions (a quad over the target) at 0, then 6 colours of
 * four bytes at 48 */
#define COLOURS 48
static const float quad[12] = { -1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1 };

static void colours(uint8_t out[24], uint8_t r, uint8_t g, uint8_t b)
{
	for (int i = 0; i < 6; i++) {
		out[4 * i] = r;
		out[4 * i + 1] = g;
		out[4 * i + 2] = b;
		out[4 * i + 3] = 255;
	}
}

static void draw(int target, GLenum mode)
{
	glBindFramebuffer(GL_FRAMEBUFFER, fbo[target]);
	glViewport(0, 0, W, H);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
	glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 0, (void *)COLOURS);
	glDrawArrays(mode, 0, mode == GL_LINES ? 6 : 6);
}

static int wanted(const char *name)
{
	return !only || !strncmp(name, only, strlen(only));
}

/* every pixel of target t (lines: the middle row, which a line crosses)
 * the colour */
static void check(const char *name, int t, uint8_t r, uint8_t g, uint8_t b, int lines)
{
	static uint8_t px[W * H * 4];
	int bad = 0, at = -1;

	glBindFramebuffer(GL_FRAMEBUFFER, fbo[t]);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	for (int i = 0; i < W * H; i++) {
		if (lines && i / W != H / 2)
			continue;
		if (abs(px[4 * i] - r) > 1 || abs(px[4 * i + 1] - g) > 1 || abs(px[4 * i + 2] - b) > 1) {
			if (at < 0)
				at = i;
			bad++;
		}
	}
	cases++;
	if (bad && !lines) {
		failed++;
		printf("%-10s WRONG: %d pixels, the first at %d,%d: %d %d %d, wanted %d %d %d\n", name,
		       bad, at % W, at / W, px[4 * at], px[4 * at + 1], px[4 * at + 2], r, g, b);
	} else if (lines && bad == W) {
		failed++;
		printf("%-10s WRONG: no pixel of the line %d %d %d\n", name, r, g, b);
	} else {
		printf("%-10s ok\n", name);
	}
}

static void clear(int t)
{
	glBindFramebuffer(GL_FRAMEBUFFER, fbo[t]);
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
}

/* columns [x0, x1) of target t the colour */
static void check_cols(const char *name, int t, int x0, int x1, uint8_t r, uint8_t g, uint8_t b)
{
	static uint8_t px[W * H * 4];
	int bad = 0, at = -1;

	glBindFramebuffer(GL_FRAMEBUFFER, fbo[t]);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	for (int i = 0; i < W * H; i++) {
		if (i % W < x0 || i % W >= x1)
			continue;
		if (abs(px[4 * i] - r) > 1 || abs(px[4 * i + 1] - g) > 1 || abs(px[4 * i + 2] - b) > 1) {
			if (at < 0)
				at = i;
			bad++;
		}
	}
	cases++;
	if (bad) {
		failed++;
		printf("%-10s WRONG: %d pixels, the first at %d,%d: %d %d %d, wanted %d %d %d\n", name,
		       bad, at % W, at / W, px[4 * at], px[4 * at + 1], px[4 * at + 2], r, g, b);
	} else {
		printf("%-10s ok\n", name);
	}
}

/* (M46) PPSSPP's push buffers: vertices x y + colour (12 bytes) and 16-bit
 * indices, each buffer mapped whole with GL_MAP_INVALIDATE_BUFFER_BIT */
struct vtx { float x, y; uint8_t c[4]; };
#define PUSH_V 65536
#define PUSH_I 8192
static GLuint pvb, pib;
static GLsizeiptr pvb_size = PUSH_V, pib_size = PUSH_I;

/* a quad (x0..x1 across, the whole height; lines: all of it at y 0.03, the
 * middle row) of vertices at voff and its six indices at ioff, both
 * buffers' old content invalidated; false if a map failed */
static int push_quad(GLintptr voff, GLintptr ioff, float x0, float x1, int lines, uint8_t r,
		     uint8_t g, uint8_t b)
{
	static const uint16_t q[6] = { 0, 1, 2, 0, 2, 3 };
	const GLbitfield inval = GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT;
	struct vtx *v;
	uint8_t *ix;

	glBindBuffer(GL_ARRAY_BUFFER, pvb);
	v = glMapBufferRange(GL_ARRAY_BUFFER, 0, pvb_size, inval);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, pib);
	ix = glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, pib_size, inval);
	if (v) {
		v = (struct vtx *)((uint8_t *)v + voff);
		for (int i = 0; i < 4; i++) {
			v[i].x = i == 1 || i == 2 ? x1 : x0;
			v[i].y = lines ? 0.03f : i >= 2 ? 1 : -1;
			v[i].c[0] = r, v[i].c[1] = g, v[i].c[2] = b, v[i].c[3] = 255;
		}
	}
	if (ix)
		memcpy(ix + ioff, q, sizeof(q));
	if (ix)
		glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
	glBindBuffer(GL_ARRAY_BUFFER, pvb);
	if (v)
		glUnmapBuffer(GL_ARRAY_BUFFER);
	return v && ix;
}

static void push_draw(int t, GLintptr voff, GLintptr ioff, GLenum mode)
{
	glBindFramebuffer(GL_FRAMEBUFFER, fbo[t]);
	glViewport(0, 0, W, H);
	glBindBuffer(GL_ARRAY_BUFFER, pvb);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, pib);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(struct vtx), (void *)voff);
	glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(struct vtx), (void *)(voff + 8));
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glDrawElements(mode, 6, GL_UNSIGNED_SHORT, (void *)ioff);
}

static void map_failed(const char *name)
{
	cases++;
	failed++;
	printf("%-10s WRONG: glMapBufferRange failed\n", name);
}

static void invalidate_cases(void)
{
	glGenBuffers(1, &pvb);
	glGenBuffers(1, &pib);
	glBindBuffer(GL_ARRAY_BUFFER, pvb);
	glBufferData(GL_ARRAY_BUFFER, PUSH_V, NULL, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, pib);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, PUSH_I, NULL, GL_DYNAMIC_DRAW);
	glFinish();

	/* the first map: no GPU copy yet */
	clear(0);
	if (!push_quad(0, 0, -1, 1, 0, 230, 20, 20))
		map_failed("inv first");
	push_draw(0, 0, 0, GL_TRIANGLES);
	glFinish();
	check("inv first", 0, 230, 20, 20, 0);

	/* mapped again while the draw that read it is gathered (not rendered),
	 * a draw into the other target: each its own */
	clear(0);
	clear(1);
	push_quad(0, 0, -1, 1, 0, 20, 230, 20);
	push_draw(0, 0, 0, GL_TRIANGLES);
	if (!push_quad(0, 0, -1, 1, 0, 20, 20, 230))
		map_failed("inv gath");
	push_draw(1, 0, 0, GL_TRIANGLES);
	glFinish();
	check("inv gath 1", 0, 20, 230, 20, 0);
	check("inv gath 2", 1, 20, 20, 230, 0);

	/* the same with the first render sent off (running, or done) */
	clear(0);
	clear(1);
	push_quad(0, 0, -1, 1, 0, 230, 230, 20);
	push_draw(0, 0, 0, GL_TRIANGLES);
	glFlush();
	push_quad(0, 0, -1, 1, 0, 230, 20, 230);
	push_draw(1, 0, 0, GL_TRIANGLES);
	glFinish();
	check("inv run 1", 0, 230, 230, 20, 0);
	check("inv run 2", 1, 230, 20, 230, 0);

	/* idle: written in place */
	clear(0);
	push_quad(0, 0, -1, 1, 0, 20, 230, 230);
	push_draw(0, 0, 0, GL_TRIANGLES);
	glFinish();
	check("inv idle", 0, 20, 230, 230, 0);

	/* at offsets: the vertices 4 KiB + 48 in, the indices at 1002 bytes
	 * (an odd first index, as PPSSPP's 2-byte aligned ones come) */
	clear(0);
	push_quad(4096 + 48, 1002, -1, 1, 0, 120, 60, 200);
	push_draw(0, 4096 + 48, 1002, GL_TRIANGLES);
	glFinish();
	check("inv offset", 0, 120, 60, 200, 0);

	/* twice in one render (PPSSPP after a sync mid-frame): the left half
	 * at 0, the buffers mapped again, the right half further on -- the
	 * second map's copy has nothing at 0, which no draw reads after it */
	clear(0);
	push_quad(0, 0, -1, 0, 0, 200, 100, 0);
	push_draw(0, 0, 0, GL_TRIANGLES);
	push_quad(480, 64, 0, 1, 0, 0, 100, 200);
	push_draw(0, 480, 64, GL_TRIANGLES);
	glFinish();
	check_cols("inv twice L", 0, 0, W / 2, 200, 100, 0);
	check_cols("inv twice R", 0, W / 2, W, 0, 100, 200);

	/* read back: what the map wrote (the CPU copy brought up to date) */
	{
		struct vtx back[4];
		int bad = 0;

		push_quad(256, 0, -1, 1, 0, 11, 22, 33);
		glBindBuffer(GL_ARRAY_BUFFER, pvb);
		glGetBufferSubData(GL_ARRAY_BUFFER, 256, sizeof(back), back);
		for (int i = 0; i < 4; i++)
			bad |= back[i].c[0] != 11 || back[i].c[1] != 22 || back[i].c[2] != 33 ||
			       back[i].x != (i == 1 || i == 2 ? 1.0f : -1.0f);
		cases++;
		if (bad) {
			failed++;
			printf("%-10s WRONG: the bytes read back are not the ones mapped and written\n",
			       "inv readbk");
		} else {
			printf("%-10s ok\n", "inv readbk");
		}
	}

	/* lines: the draw module reads the vertices and indices on the CPU */
	clear(0);
	push_quad(0, 0, -1, 1, 1, 255, 128, 0);
	push_draw(0, 0, 0, GL_LINES);
	glFinish();
	check("inv lines", 0, 255, 128, 0, 1);

	/* glBufferData of all of a buffer a gathered draw reads: Mesa's
	 * buffer_subdata of the whole, the driver's map of a fresh GPU copy */
	{
		static struct vtx v[PUSH_V / sizeof(struct vtx)];

		clear(0);
		clear(1);
		push_quad(0, 0, -1, 1, 0, 90, 200, 90);
		push_draw(0, 0, 0, GL_TRIANGLES);
		for (int i = 0; i < 4; i++) {
			v[i].x = i == 1 || i == 2 ? 1 : -1;
			v[i].y = i >= 2 ? 1 : -1;
			v[i].c[0] = 200, v[i].c[1] = 90, v[i].c[2] = 200, v[i].c[3] = 255;
		}
		glBindBuffer(GL_ARRAY_BUFFER, pvb);
		glBufferData(GL_ARRAY_BUFFER, PUSH_V, v, GL_DYNAMIC_DRAW);
		push_draw(1, 0, 0, GL_TRIANGLES);
		glFinish();
		check("inv data 1", 0, 90, 200, 90, 0);
		check("inv data 2", 1, 200, 90, 200, 0);
	}

	/* a buffer under 4 KiB (the CPU copy, as before), mapped again while a
	 * draw reads it */
	pvb_size = 1024;
	pib_size = 256;
	glBindBuffer(GL_ARRAY_BUFFER, pvb);
	glBufferData(GL_ARRAY_BUFFER, pvb_size, NULL, GL_DYNAMIC_DRAW);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, pib);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, pib_size, NULL, GL_DYNAMIC_DRAW);
	clear(0);
	clear(1);
	push_quad(0, 0, -1, 1, 0, 60, 60, 60);
	push_draw(0, 0, 0, GL_TRIANGLES);
	push_quad(0, 0, -1, 1, 0, 160, 160, 60);
	push_draw(1, 0, 0, GL_TRIANGLES);
	glFinish();
	check("inv small 1", 0, 60, 60, 60, 0);
	check("inv small 2", 1, 160, 160, 60, 0);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

int main(int argc, char **argv)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_MAJOR_VERSION, 2, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE };
	EGLDisplay dpy;
	EGLContext ctx;
	uint8_t c[24];

	only = argc > 1 ? argv[1] : NULL;
	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_API))
		return fprintf(stderr, "glbuffer: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glbuffer: no OpenGL context\n"), 2;

	glGenTextures(2, rt);
	glGenFramebuffers(2, fbo);
	for (int i = 0; i < 2; i++) {
		glBindTexture(GL_TEXTURE_2D, rt[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt[i], 0);
	}
	prog = glCreateProgram();
	{
		GLuint s[2] = { glCreateShader(GL_VERTEX_SHADER), glCreateShader(GL_FRAGMENT_SHADER) };

		glShaderSource(s[0], 1, &vs, NULL);
		glShaderSource(s[1], 1, &fs, NULL);
		for (int i = 0; i < 2; i++) {
			glCompileShader(s[i]);
			glAttachShader(prog, s[i]);
		}
	}
	glBindAttribLocation(prog, 0, "p");
	glBindAttribLocation(prog, 1, "c");
	glLinkProgram(prog);
	glUseProgram(prog);

	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, 256, NULL, GL_DYNAMIC_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
	colours(c, 200, 10, 10);
	glBufferSubData(GL_ARRAY_BUFFER, COLOURS, sizeof(c), c);
	/* (a first draw: the buffer gets its GPU copy) */
	clear(0);
	draw(0, GL_TRIANGLES);
	glFinish();
	check("first", 0, 200, 10, 10, 0);

	if (wanted("direct")) {
		/* the whole buffer's 72 bytes again (64 or more: the direct way),
		 * the colours new */
		uint8_t all[72];

		memcpy(all, quad, sizeof(quad));
		colours(all + COLOURS, 10, 200, 10);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(all), all);
		clear(0);
		draw(0, GL_TRIANGLES);
		glFinish();
		check("direct", 0, 10, 200, 10, 0);
	}
	if (wanted("readback")) {
		uint8_t all[72], back[72];

		memcpy(all, quad, sizeof(quad));
		colours(all + COLOURS, 33, 66, 99);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(all), all);
		glGetBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(back), back);
		cases++;
		if (memcmp(all, back, sizeof(all))) {
			failed++;
			printf("%-10s WRONG: the bytes read back are not the ones written\n", "readback");
		} else {
			printf("%-10s ok\n", "readback");
		}
	}
	if (wanted("busy")) {
		/* a draw gathered (not rendered: no flush), the colours written
		 * over, a draw into the other target: the first keeps its own */
		uint8_t all[72];

		glFinish();
		clear(0);
		clear(1);
		memcpy(all, quad, sizeof(quad));
		colours(all + COLOURS, 250, 250, 10);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(all), all);
		draw(0, GL_TRIANGLES);
		colours(all + COLOURS, 10, 250, 250);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(all), all);
		draw(1, GL_TRIANGLES);
		glFinish();
		check("busy 1st", 0, 250, 250, 10, 0);
		check("busy 2nd", 1, 10, 250, 250, 0);
	}
	if (wanted("fetched")) {
		/* x y and the colour interleaved, one stride: the GPU fetches the
		 * vertices from the buffer's GPU copy itself (M26), written
		 * straight -- a draw, glFinish, new colours, a draw */
		struct { float x, y; uint8_t c[4]; } v[6];
		GLuint vbo2;

		for (int i = 0; i < 6; i++) {
			v[i].x = quad[2 * i];
			v[i].y = quad[2 * i + 1];
			v[i].c[0] = 40, v[i].c[1] = 80, v[i].c[2] = 160, v[i].c[3] = 255;
		}
		glGenBuffers(1, &vbo2);
		glBindBuffer(GL_ARRAY_BUFFER, vbo2);
		glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_DYNAMIC_DRAW);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(v[0]), (void *)0);
		glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(v[0]), (void *)8);
		glEnableVertexAttribArray(0);
		glEnableVertexAttribArray(1);
		clear(0);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glFinish();
		check("fetched 1", 0, 40, 80, 160, 0);
		for (int i = 0; i < 6; i++)
			v[i].c[0] = 220, v[i].c[1] = 20, v[i].c[2] = 120;
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
		clear(0);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glFinish();
		check("fetched 2", 0, 220, 20, 120, 0);
		/* gathered and written over before it is rendered */
		clear(0);
		clear(1);
		for (int i = 0; i < 6; i++)
			v[i].c[0] = 5, v[i].c[1] = 240, v[i].c[2] = 5;
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		for (int i = 0; i < 6; i++)
			v[i].c[0] = 240, v[i].c[1] = 5, v[i].c[2] = 240;
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo[1]);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glFinish();
		check("fetched 3", 0, 5, 240, 5, 0);
		check("fetched 4", 1, 240, 5, 240, 0);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
	}
	if (wanted("indexed")) {
		/* glDrawElements from an index buffer (16 bits, a list): at 0,
		 * at a 4-byte offset, at a 2-byte one (the first index odd), and
		 * after the indices were written again -- the VDM reads them in the
		 * buffer's GPU copy (M46) -- then read back */
		struct { float x, y; uint8_t c[4]; } v[8];
		uint16_t idx[24] = { 0, 1, 2, 0, 2, 3,  4, 5, 6, 4, 6, 7,  0,  4, 5, 6, 4, 6, 7,
				     0, 0, 0, 0, 0 }, back[16];
		GLuint vb, ib;

		/* two quads: the whole target red-ish, then (vertices 4..7) the
		 * whole target blue-ish */
		for (int i = 0; i < 8; i++) {
			v[i].x = (i & 3) == 1 || (i & 3) == 2 ? 1 : -1;
			v[i].y = (i & 3) >= 2 ? 1 : -1;
			v[i].c[0] = i < 4 ? 210 : 20;
			v[i].c[1] = 30;
			v[i].c[2] = i < 4 ? 20 : 210;
			v[i].c[3] = 255;
		}
		glFinish();
		glGenBuffers(1, &vb);
		glGenBuffers(1, &ib);
		glBindBuffer(GL_ARRAY_BUFFER, vb);
		glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_DYNAMIC_DRAW);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_DYNAMIC_DRAW);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(v[0]), (void *)0);
		glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(v[0]), (void *)8);
		glEnableVertexAttribArray(0);
		glEnableVertexAttribArray(1);
		clear(0);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void *)0);
		glFinish();
		check("indexed 0", 0, 210, 30, 20, 0);
		clear(0);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void *)12);
		glFinish();
		check("indexed 12", 0, 20, 30, 210, 0);
		clear(0);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void *)26);
		glFinish();
		check("indexed 26", 0, 20, 30, 210, 0);
		/* the indices written again (all of them, 32 bytes: under the
		 * direct write's 64, then the whole 64 of a bigger write), the
		 * first six now the second quad's */
		{
			uint16_t big[32];

			for (int i = 0; i < 32; i++)
				big[i] = i < 12 ? idx[(i + 6) % 12] : 0;
			glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(big), NULL, GL_DYNAMIC_DRAW);
			glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, sizeof(big), big);
			clear(0);
			glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
			glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void *)0);
			glFinish();
			check("indexed new", 0, 20, 30, 210, 0);
			for (int i = 0; i < 12; i++)
				big[i] = idx[i];
			glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, sizeof(big), big);
			clear(0);
			glBindFramebuffer(GL_FRAMEBUFFER, fbo[0]);
			glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, (void *)0);
			glFinish();
			check("indexed again", 0, 210, 30, 20, 0);
			glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, sizeof(back), back);
			cases++;
			if (memcmp(back, big, sizeof(back))) {
				failed++;
				printf("%-10s WRONG: the indices read back are not the ones written\n",
				       "indexed rb");
			} else {
				printf("%-10s ok\n", "indexed rb");
			}
		}
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
	}
	if (wanted("lines")) {
		/* lines across the middle row, the draw module's (the CPU reads the
		 * buffer it was written straight into) */
		static const float line[12] = { -1, 0.03f, 1, 0.03f, -1, 0.03f, 1, 0.03f, -1, 0.03f, 1, 0.03f };
		uint8_t all[72];

		glFinish();
		memcpy(all, quad, sizeof(quad));
		colours(all + COLOURS, 180, 180, 180);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(all), all);
		draw(0, GL_TRIANGLES);
		glFinish();
		memcpy(all, line, sizeof(line));
		colours(all + COLOURS, 255, 128, 0);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(all), all);
		clear(0);
		draw(0, GL_LINES);
		glFinish();
		check("lines", 0, 255, 128, 0, 1);
	}
	if (wanted("map")) {
		/* back to the quad (a direct write), then the colours through a
		 * map of that range alone */
		uint8_t all[72], *m;

		glFinish();
		memcpy(all, quad, sizeof(quad));
		colours(all + COLOURS, 1, 2, 3);
		glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(all), all);
		m = glMapBufferRange(GL_ARRAY_BUFFER, COLOURS, 24, GL_MAP_WRITE_BIT);
		if (m) {
			colours(m, 90, 160, 230);
			glUnmapBuffer(GL_ARRAY_BUFFER);
		}
		clear(0);
		draw(0, GL_TRIANGLES);
		glFinish();
		check("map", 0, 90, 160, 230, 0);
	}
	if (wanted("invalidate"))
		invalidate_cases();
	printf("%d of %d cases right\n", cases - failed, cases);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return failed != 0;
}
