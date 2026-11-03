/* glvtx -- the vertex path (docs/research/p105-mesa.md, M26): vertices
 * fetched from the application's buffers as they are, or made one stream
 * by the CPU.  A grid of 8 x 8 cells, a quad in a colour of its own in
 * each one a case draws; each cell's middle read back.  The cases: SDL's
 * layout (two floats, four floats of colour, stride 24, a draw a quad from
 * one buffer), colours as bytes (stride 12), attributes in two buffers with
 * one stride, with two strides, a constant colour, a buffer written again
 * between two draws of one render, indices from a buffer and from memory
 * (bytes), a strip from a vertex past the first, RGB bytes, short colours,
 * 64 quads a draw each.
 *
 *   sgx-gl glvtx
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 128
#define H 128

static uint8_t px[W * H * 4];
static uint8_t want[64][4];      /* each cell's colour; alpha 0: nothing drawn there */
static int cases, failed;

static void
colour(unsigned i, uint8_t c[4])
{
	c[0] = (i * 37 + 20) & 255;
	c[1] = (i * 91 + 60) & 255;
	c[2] = (i * 53 + 100) & 255;
	c[3] = 255;
}

/* cell i's quad: six corners, x y */
static void
corners(unsigned i, float xy[12])
{
	float x0 = (i % 8) * 16 + 3, y0 = (i / 8) * 16 + 3, x1 = x0 + 10, y1 = y0 + 10;
	const float c[6][2] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y0 }, { x1, y1 }, { x0, y1 } };

	for (int k = 0; k < 6; k++) {
		xy[2 * k] = c[k][0] / (W / 2) - 1;
		xy[2 * k + 1] = c[k][1] / (H / 2) - 1;
	}
}

static void
expect(unsigned i)
{
	colour(i, want[i]);
}

static void
start(void)
{
	memset(want, 0, sizeof(want));
	glClear(GL_COLOR_BUFFER_BIT);
}

static void
check(const char *name)
{
	int bad = 0, first = -1;

	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	for (int i = 0; i < 64; i++) {
		const uint8_t *p = px + (((i / 8) * 16 + 8) * W + (i % 8) * 16 + 8) * 4;

		for (int c = 0; c < 4; c++)
			if (abs(p[c] - want[i][c]) > 3) {
				bad++;
				if (first < 0)
					first = i;
				break;
			}
	}
	cases++;
	failed += bad != 0;
	if (!bad) {
		printf("%-44s ok\n", name);
	} else {
		const uint8_t *p = px + (((first / 8) * 16 + 8) * W + (first % 8) * 16 + 8) * 4;

		printf("%-44s WRONG: %d cells, the first %d: %02x %02x %02x %02x, want %02x %02x %02x %02x\n",
		       name, bad, first, p[0], p[1], p[2], p[3], want[first][0], want[first][1],
		       want[first][2], want[first][3]);
	}
}

static GLuint
program(void)
{
	static const char *vs = "attribute vec2 pos; attribute vec4 col; varying vec4 c;\n"
	                        "void main() { c = col; gl_Position = vec4(pos, 0.0, 1.0); }\n";
	static const char *fs = "precision mediump float; varying vec4 c;\n"
	                        "void main() { gl_FragColor = c; }\n";
	GLuint p = glCreateProgram();

	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);

		glShaderSource(s, 1, i ? &fs : &vs, NULL);
		glCompileShader(s);
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "pos");
	glBindAttribLocation(p, 1, "col");
	glLinkProgram(p);
	return p;
}

int main(void)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	GLuint rt, fbo, buf[4];
	EGLDisplay dpy;
	EGLContext ctx;

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glvtx: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glvtx: no GLES 2 context\n"), 2;
	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glViewport(0, 0, W, H);
	glUseProgram(program());
	glGenBuffers(4, buf);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);

	/* SDL's: x y, then r g b a as floats, stride 24, one buffer for the
	 * frame, a draw a quad (its attributes pointed at it) */
	{
		float v[16][6][6];

		for (int q = 0; q < 16; q++) {
			float xy[12];
			uint8_t c[4];

			corners(q * 4 % 64 + q / 16, xy);
			colour(q * 4 % 64 + q / 16, c);
			for (int k = 0; k < 6; k++) {
				v[q][k][0] = xy[2 * k];
				v[q][k][1] = xy[2 * k + 1];
				for (int j = 0; j < 4; j++)
					v[q][k][2 + j] = c[j] / 255.0f;
			}
		}
		start();
		glBindBuffer(GL_ARRAY_BUFFER, buf[0]);
		glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STREAM_DRAW);
		for (int q = 0; q < 16; q++) {
			glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 24, (void *)(uintptr_t)(q * 6 * 24));
			glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 24, (void *)(uintptr_t)(q * 6 * 24 + 8));
			glDrawArrays(GL_TRIANGLES, 0, 6);
			expect(q * 4 % 64 + q / 16);
		}
		check("floats, stride 24, a draw a quad (SDL's)");
	}

	/* colours as bytes: x y, r g b a, stride 12 */
	{
		struct { float x, y; uint8_t c[4]; } v[8][6];

		for (int q = 0; q < 8; q++) {
			float xy[12];
			uint8_t c[4];

			corners(q * 8 + 1, xy);
			colour(q * 8 + 1, c);
			for (int k = 0; k < 6; k++) {
				v[q][k].x = xy[2 * k];
				v[q][k].y = xy[2 * k + 1];
				memcpy(v[q][k].c, c, 4);
			}
		}
		start();
		glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STREAM_DRAW);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 12, (void *)0);
		glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, 12, (void *)8);
		glDrawArrays(GL_TRIANGLES, 0, 48);
		for (int q = 0; q < 8; q++)
			expect(q * 8 + 1);
		check("byte colours, stride 12");
	}

	/* RGB bytes, stride 16 (alpha 1) */
	{
		struct { float x, y; uint8_t c[3], pad[5]; } v[4][6];

		for (int q = 0; q < 4; q++) {
			float xy[12];
			uint8_t c[4];

			corners(q * 9 + 2, xy);
			colour(q * 9 + 2, c);
			for (int k = 0; k < 6; k++) {
				v[q][k].x = xy[2 * k];
				v[q][k].y = xy[2 * k + 1];
				memcpy(v[q][k].c, c, 3);
				memset(v[q][k].pad, 0x55, 5);
			}
		}
		start();
		glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STREAM_DRAW);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
		glVertexAttribPointer(1, 3, GL_UNSIGNED_BYTE, GL_TRUE, 16, (void *)8);
		glDrawArrays(GL_TRIANGLES, 0, 24);
		for (int q = 0; q < 4; q++)
			expect(q * 9 + 2);
		check("RGB byte colours, stride 16");
	}

	/* positions and colours in two buffers, one stride (16) */
	{
		float pos[6][4], col[6][4], xy[12];
		uint8_t c[4];

		corners(10, xy);
		colour(10, c);
		for (int k = 0; k < 6; k++) {
			pos[k][0] = xy[2 * k];
			pos[k][1] = xy[2 * k + 1];
			pos[k][2] = pos[k][3] = 7;
			for (int j = 0; j < 4; j++)
				col[k][j] = c[j] / 255.0f;
		}
		start();
		glBufferData(GL_ARRAY_BUFFER, sizeof(pos), pos, GL_STREAM_DRAW);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
		glBindBuffer(GL_ARRAY_BUFFER, buf[1]);
		glBufferData(GL_ARRAY_BUFFER, sizeof(col), col, GL_STREAM_DRAW);
		glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 16, (void *)0);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		expect(10);
		check("two buffers, one stride");

		/* two strides: positions packed (8), colours 16 */
		{
			float p2[6][2];

			for (int k = 0; k < 6; k++)
				memcpy(p2[k], pos[k], 8);
			start();
			glBindBuffer(GL_ARRAY_BUFFER, buf[2]);
			glBufferData(GL_ARRAY_BUFFER, sizeof(p2), p2, GL_STREAM_DRAW);
			glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
			glDrawArrays(GL_TRIANGLES, 0, 6);
			expect(10);
			check("two buffers, two strides");
		}

		/* a constant colour */
		start();
		glDisableVertexAttribArray(1);
		colour(11, c);
		glVertexAttrib4f(1, c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, 1);
		corners(11, xy);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, xy);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		expect(11);
		check("a constant colour");
		glEnableVertexAttribArray(1);
	}

	/* a buffer written again between two draws of one render: the first
	 * draw keeps what it was given */
	{
		float v[6][6];

		start();
		glBindBuffer(GL_ARRAY_BUFFER, buf[3]);
		for (int round = 0; round < 3; round++) {
			float xy[12];
			uint8_t c[4];
			int cell = 20 + round * 7;

			corners(cell, xy);
			colour(cell, c);
			for (int k = 0; k < 6; k++) {
				v[k][0] = xy[2 * k];
				v[k][1] = xy[2 * k + 1];
				for (int j = 0; j < 4; j++)
					v[k][2 + j] = c[j] / 255.0f;
			}
			if (!round)
				glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_DYNAMIC_DRAW);
			else
				glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(v), v);
			glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 24, (void *)0);
			glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 24, (void *)8);
			glDrawArrays(GL_TRIANGLES, 0, 6);
			expect(cell);
		}
		check("a buffer written again between draws");
	}

	/* indices: from a buffer (unsigned short, an offset into it), and
	 * from memory (bytes); a strip from vertex 4 */
	{
		float v[3][4][6];
		static const uint16_t idx16[] = { 99, 99, 99, 4, 5, 6, 4, 6, 7, 8, 9, 10, 8, 10, 11 };
		static const uint8_t idx8[] = { 0, 1, 2, 0, 2, 3 };

		for (int q = 0; q < 3; q++) {
			float xy[12];
			uint8_t c[4];
			static const int at[] = { 0, 1, 2, 5 };   /* the quad's corners in corners() */

			corners(40 + q, xy);
			colour(40 + q, c);
			for (int k = 0; k < 4; k++) {
				v[q][k][0] = xy[2 * at[k]];
				v[q][k][1] = xy[2 * at[k] + 1];
				for (int j = 0; j < 4; j++)
					v[q][k][2 + j] = c[j] / 255.0f;
			}
		}
		start();
		glBindBuffer(GL_ARRAY_BUFFER, buf[0]);
		glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STATIC_DRAW);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buf[1]);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx16), idx16, GL_STATIC_DRAW);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 24, (void *)0);
		glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 24, (void *)8);
		glDrawElements(GL_TRIANGLES, 12, GL_UNSIGNED_SHORT, (void *)6);
		expect(41);
		expect(42);
		check("indices from a buffer, at an offset");

		start();
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_BYTE, idx8);
		expect(40);
		check("byte indices from memory");

		/* the quad as a strip: 0 1 3 2 */
		{
			float s[4][6];
			static const int order[] = { 0, 1, 3, 2 };

			for (int k = 0; k < 4; k++)
				memcpy(s[k], v[2][order[k]], sizeof(s[k]));
			start();
			glBindBuffer(GL_ARRAY_BUFFER, buf[2]);
			glBufferData(GL_ARRAY_BUFFER, sizeof(v[0]) + sizeof(s), NULL, GL_STATIC_DRAW);
			glBufferSubData(GL_ARRAY_BUFFER, sizeof(v[0]), sizeof(s), s);
			glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 24, (void *)0);
			glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 24, (void *)8);
			glDrawArrays(GL_TRIANGLE_STRIP, 4, 4);
			expect(42);
			check("a strip from vertex 4");
		}
	}

	/* colours as shorts, normalized (the CPU makes them floats) */
	{
		struct { float x, y; int16_t c[4]; } v[6];
		float xy[12];
		uint8_t c[4];

		corners(50, xy);
		colour(50, c);
		for (int k = 0; k < 6; k++) {
			v[k].x = xy[2 * k];
			v[k].y = xy[2 * k + 1];
			for (int j = 0; j < 4; j++)
				v[k].c[j] = c[j] * 32767 / 255;
		}
		start();
		glBindBuffer(GL_ARRAY_BUFFER, buf[0]);
		glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STREAM_DRAW);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
		glVertexAttribPointer(1, 4, GL_SHORT, GL_TRUE, 16, (void *)8);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		expect(50);
		check("short colours");
	}

	/* 64 quads, a draw each, one after another in one buffer */
	{
		float v[64][6][6];

		for (int q = 0; q < 64; q++) {
			float xy[12];
			uint8_t c[4];

			corners(q, xy);
			colour(q, c);
			for (int k = 0; k < 6; k++) {
				v[q][k][0] = xy[2 * k];
				v[q][k][1] = xy[2 * k + 1];
				for (int j = 0; j < 4; j++)
					v[q][k][2 + j] = c[j] / 255.0f;
			}
		}
		start();
		glBindBuffer(GL_ARRAY_BUFFER, buf[3]);
		glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STREAM_DRAW);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 24, (void *)0);
		glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 24, (void *)8);
		for (int q = 0; q < 64; q++) {
			glDrawArrays(GL_TRIANGLES, q * 6, 6);
			expect(q);
		}
		check("64 quads, a draw each");
	}

	printf("%d of %d cases right\n", cases - failed, cases);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return failed ? 1 : 0;
}
