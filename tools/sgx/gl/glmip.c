/* glmip -- mipmaps and cube maps (docs/research/p105-mesa.md, M28).
 *
 * Mipmaps: a 64 x 64 texture whose levels are each a colour of their own
 * (and the texel at (0, 0) of each a second one), drawn as quads of 64,
 * 32, 16, ... pixels: each should show its own level, nearest within the
 * level and between levels (NEAREST_MIPMAP_NEAREST).  Then the same with a
 * bias in the shader, one level further each time, and GL's
 * glGenerateMipmap'd levels of a two-colour texture (the average).
 *
 * Cube maps: a 4 x 4 cube map, each face a colour of its own and its
 * texel (s, t) a little of s and t in the other channels; a quad a face,
 * the direction through each texel's middle in turn: which face and
 * texel comes back; then a 32 x 32 one with every level, and sizes not a
 * power of two.
 *
 *   sgx-gl glmip [--dump]
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 256
#define H 256

static uint8_t px[W * H * 4];
static int cases, failed, dump;

static const uint8_t level_colour[8][3] = {
	{ 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 0 },
	{ 0, 255, 255 }, { 255, 0, 255 }, { 128, 128, 128 }, { 255, 255, 255 },
};

static GLuint
program(const char *vs, const char *fs)
{
	GLuint p = glCreateProgram();
	char log[512];
	GLint ok;

	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);

		glShaderSource(s, 1, i ? &fs : &vs, NULL);
		glCompileShader(s);
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			glGetShaderInfoLog(s, sizeof(log), NULL, log);
			fprintf(stderr, "glmip: %s\n", log);
		}
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "p");
	glBindAttribLocation(p, 1, "a");
	glLinkProgram(p);
	return p;
}

/* a quad over pixels [x, x + w) x [y, y + h), its attribute a per corner */
static void
quad(int x, int y, int w, int h, const float a[4][3])
{
	float x0 = 2.0f * x / W - 1, x1 = 2.0f * (x + w) / W - 1;
	float y0 = 2.0f * y / H - 1, y1 = 2.0f * (y + h) / H - 1;
	const float pos[6][2] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y0 }, { x1, y1 }, { x0, y1 } };
	static const int corner[6] = { 0, 1, 2, 0, 2, 3 };
	float attr[6][3];

	for (int i = 0; i < 6; i++)
		memcpy(attr[i], a[corner[i]], sizeof(attr[i]));
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, attr);
	glDrawArrays(GL_TRIANGLES, 0, 6);
}

static const uint8_t *
at(int x, int y)
{
	return px + (y * W + x) * 4;
}

static int
near(const uint8_t *p, const uint8_t *want, int tol)
{
	for (int c = 0; c < 3; c++)
		if (abs(p[c] - want[c]) > tol)
			return 0;
	return 1;
}

static void
check(const char *name, int ok, const char *detail)
{
	cases++;
	failed += !ok;
	printf("%-48s %s%s%s\n", name, ok ? "ok" : "WRONG", detail[0] ? ": " : "", detail);
}

/* which level colour p is, -1 none */
static int
which_level(const uint8_t *p)
{
	for (int l = 0; l < 8; l++)
		if (near(p, level_colour[l], 24))
			return l;
	return -1;
}

static void
mipmaps(void)
{
	static const char *vs = "attribute vec2 p; attribute vec3 a; varying vec2 t;\n"
	                        "void main() { t = a.xy; gl_Position = vec4(p, 0.0, 1.0); }\n";
	static const char *fs = "precision mediump float; varying vec2 t; uniform sampler2D s;\n"
	                        "uniform float bias;\n"
	                        "void main() { gl_FragColor = texture2D(s, t, bias); }\n";
	GLuint p = program(vs, fs), tex;
	static const float uv[4][3] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	char detail[256];

	glUseProgram(p);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	for (int l = 0; l < 7; l++) {
		int n = 64 >> l;
		uint8_t *t = malloc(n * n * 4);

		for (int i = 0; i < n * n; i++) {
			memcpy(t + 4 * i, level_colour[l], 3);
			t[4 * i + 3] = 255;
		}
		glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA, n, n, 0, GL_RGBA, GL_UNSIGNED_BYTE, t);
		free(t);
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	for (int b = 0; b < 3; b++) {
		int got[7], ok = 1, m = 0;

		glUniform1f(glGetUniformLocation(p, "bias"), (float)b);
		glClear(GL_COLOR_BUFFER_BIT);
		/* quads of 64, 32, ... 1 pixels: level 0, 1, ... */
		for (int l = 0, x = 0; l < 7; x += (64 >> l) + 4, l++)
			quad(x, 0, 64 >> l, 64 >> l, uv);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		for (int l = 0, x = 0; l < 7; x += (64 >> l) + 4, l++) {
			int want = l + b > 6 ? 6 : l + b;

			got[l] = which_level(at(x + (64 >> l) / 2, (64 >> l) / 2));
			ok &= got[l] == want;
			m += snprintf(detail + m, sizeof(detail) - m, " %d", got[l]);
		}
		check(b == 0 ? "levels, one a quad of its size" :
		      b == 1 ? "levels, bias 1" : "levels, bias 2", ok, detail);
	}
	glUniform1f(glGetUniformLocation(p, "bias"), 0.0f);

	/* without a mip filter: level 0 whatever the size */
	{
		int ok = 1, m = 0;

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glClear(GL_COLOR_BUFFER_BIT);
		for (int l = 0, x = 0; l < 7; x += (64 >> l) + 4, l++)
			quad(x, 0, 64 >> l, 64 >> l, uv);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		for (int l = 0, x = 0; l < 7; x += (64 >> l) + 4, l++) {
			int got = which_level(at(x + (64 >> l) / 2, (64 >> l) / 2));

			ok &= got == 0;
			m += snprintf(detail + m, sizeof(detail) - m, " %d", got);
		}
		check("no mip filter: level 0", ok, detail);
	}

	/* the linear mip filter: halfway between two levels, half each (a
	 * quad of 64 / sqrt(2) pixels: level 0.5), and on a level, all of it */
	{
		const uint8_t *q;

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
		glClear(GL_COLOR_BUFFER_BIT);
		quad(0, 0, 45, 45, uv);
		quad(64, 0, 32, 32, uv);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		q = at(22, 22);
		snprintf(detail, sizeof(detail), "%02x %02x %02x (half red, half green), then %d",
		         q[0], q[1], q[2], which_level(at(64 + 16, 16)));
		check("the linear mip filter", q[0] > 64 && q[0] < 192 && q[1] > 64 && q[1] < 192 &&
		      which_level(at(64 + 16, 16)) == 1, detail);
	}
	glDeleteTextures(1, &tex);
}

static void
cube(void)
{
	static const char *vs = "attribute vec2 p; attribute vec3 a; varying vec3 d;\n"
	                        "void main() { d = a; gl_Position = vec4(p, 0.0, 1.0); }\n";
	static const char *fs = "precision mediump float; varying vec3 d; uniform samplerCube c;\n"
	                        "void main() { gl_FragColor = textureCube(c, d); }\n";
	/* GL's faces: the major axis, and s and t's directions (the table in
	 * the spec, 3.8.6) */
	static const float major[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
	                                   { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	static const float sdir[6][3] = { { 0, 0, -1 }, { 0, 0, 1 }, { 1, 0, 0 },
	                                  { 1, 0, 0 }, { 1, 0, 0 }, { -1, 0, 0 } };
	static const float tdir[6][3] = { { 0, -1, 0 }, { 0, -1, 0 }, { 0, 0, 1 },
	                                  { 0, 0, -1 }, { 0, -1, 0 }, { 0, -1, 0 } };
	static const char *names[6] = { "+x", "-x", "+y", "-y", "+z", "-z" };
	GLuint p = program(vs, fs), tex;
	uint8_t texels[4 * 4 * 4];

	glUseProgram(p);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
	for (int f = 0; f < 6; f++) {
		for (int i = 0; i < 16; i++) {
			texels[4 * i] = 40 * f + 20;            /* the face */
			texels[4 * i + 1] = (i & 3) * 64 + 32;  /* s */
			texels[4 * i + 2] = (i >> 2) * 64 + 32; /* t */
			texels[4 * i + 3] = 255;
		}
		glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA, 4, 4, 0, GL_RGBA,
		             GL_UNSIGNED_BYTE, texels);
	}
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glClear(GL_COLOR_BUFFER_BIT);
	/* a 16 x 16 cell a texel of a face: the direction constant over it,
	 * through the texel's middle */
	for (int f = 0; f < 6; f++)
		for (int i = 0; i < 16; i++) {
			float sc = ((i & 3) + 0.5f) / 2 - 1, tc = ((i >> 2) + 0.5f) / 2 - 1;
			float a[4][3];

			for (int k = 0; k < 3; k++)
				a[0][k] = major[f][k] + sc * sdir[f][k] + tc * tdir[f][k];
			memcpy(a[1], a[0], sizeof(a[0]));
			memcpy(a[2], a[0], sizeof(a[0]));
			memcpy(a[3], a[0], sizeof(a[0]));
			quad((f % 3) * 80 + (i & 3) * 16, (f / 3) * 80 + (i >> 2) * 16, 16, 16, a);
		}
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	for (int f = 0; f < 6; f++) {
		char detail[256], name[64];
		int ok = 1, m = 0;

		for (int i = 0; i < 16; i++) {
			const uint8_t *q = at((f % 3) * 80 + (i & 3) * 16 + 8, (f / 3) * 80 + (i >> 2) * 16 + 8);
			int gf = (q[0] - 20 + 20) / 40, gs = q[1] / 64, gt = q[2] / 64;

			ok &= gf == f && gs == (i & 3) && gt == (i >> 2);
			if (dump || i < 4)
				m += snprintf(detail + m, sizeof(detail) - m, " %d:%d,%d", gf, gs, gt);
		}
		snprintf(name, sizeof(name), "cube face %s (face:s,t of texels 0..)", names[f]);
		check(name, ok, detail);
	}
	glDeleteTextures(1, &tex);
}

/* a 32 x 32 cube map with every level, each face and level a colour of
 * its own (red the face, green the level): each face's middle at each
 * level through a bias */
static void
cube_levels(void)
{
	static const char *vs = "attribute vec2 p; attribute vec3 a; varying vec3 d;\n"
	                        "void main() { d = a; gl_Position = vec4(p, 0.0, 1.0); }\n";
	static const char *fs = "precision mediump float; varying vec3 d; uniform samplerCube c;\n"
	                        "uniform float bias;\n"
	                        "void main() { gl_FragColor = textureCube(c, d, bias); }\n";
	static const float major[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
	                                   { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	GLuint p = program(vs, fs), tex;
	char detail[512];
	int ok = 1, m = 0;

	glUseProgram(p);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
	for (int f = 0; f < 6; f++)
		for (int l = 0; l < 6; l++) {
			int n = 32 >> l;
			uint8_t *t = malloc(n * n * 4);

			for (int i = 0; i < n * n; i++) {
				t[4 * i] = 40 * f + 20;
				t[4 * i + 1] = 40 * l + 20;
				t[4 * i + 2] = 0;
				t[4 * i + 3] = 255;
			}
			glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, l, GL_RGBA, n, n, 0, GL_RGBA,
			             GL_UNSIGNED_BYTE, t);
			free(t);
		}
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	/* a quad of 16 pixels over a face's whole: level 1 (32 texels across
	 * it, two a pixel), then bias 0..4 more */
	for (int b = 0; b < 5; b++) {
		glUniform1f(glGetUniformLocation(p, "bias"), (float)b);
		glClear(GL_COLOR_BUFFER_BIT);
		for (int f = 0; f < 6; f++) {
			float a[4][3];
			static const float corner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };

			for (int k = 0; k < 4; k++)
				for (int j = 0; j < 3; j++) {
					/* the two axes other than the major one */
					int u = (j + 1) % 3, v = (j + 2) % 3;

					(void)u; (void)v;
					a[k][j] = major[f][j];
				}
			for (int k = 0; k < 4; k++) {
				int ax = major[f][0] != 0 ? 0 : major[f][1] != 0 ? 1 : 2;
				int u = (ax + 1) % 3, v = (ax + 2) % 3;

				a[k][u] = corner[k][0];
				a[k][v] = corner[k][1];
			}
			quad(f * 20, 0, 16, 16, a);
		}
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		for (int f = 0; f < 6; f++) {
			const uint8_t *q = at(f * 20 + 8, 8);
			int gf = q[0] / 40, gl = q[1] / 40, want = b + 1 > 5 ? 5 : b + 1;

			ok &= gf == f && gl == want;
			m += snprintf(detail + m, sizeof(detail) - m, " %d:%d", gf, gl);
		}
		m += snprintf(detail + m, sizeof(detail) - m, " |");
	}
	check("cube map levels (face:level, bias 0..4)", ok, detail);
	glDeleteTextures(1, &tex);
}

/* sizes not a power of two (M28): a 24 x 12 whose texels each say where
 * they are, at their middles; a 48 x 48 of one-texel black and white
 * squares minified by two, linear (grey); a 6 x 6 cube map as cube() */
static void
npot(void)
{
	static const char *vs = "attribute vec2 p; attribute vec3 a; varying vec3 d;\n"
	                        "void main() { d = a; gl_Position = vec4(p, 0.0, 1.0); }\n";
	static const char *fs2 = "precision mediump float; varying vec3 d; uniform sampler2D s;\n"
	                         "void main() { gl_FragColor = texture2D(s, d.xy); }\n";
	static const char *fsc = "precision mediump float; varying vec3 d; uniform samplerCube c;\n"
	                         "void main() { gl_FragColor = textureCube(c, d); }\n";
	GLuint p2 = program(vs, fs2), pc = program(vs, fsc), tex;
	uint8_t t[48 * 48 * 4];
	char detail[256];
	int bad = 0, first = -1;

	/* 24 x 12: texel (x, y) red 10 x, green 20 y; a 2 x 2-pixel quad a
	 * texel, its coordinates the texel's middle */
	glUseProgram(p2);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	for (int y = 0; y < 12; y++)
		for (int x = 0; x < 24; x++) {
			uint8_t *c = t + 4 * (y * 24 + x);

			c[0] = 10 * x;
			c[1] = 20 * y;
			c[2] = 99;
			c[3] = 255;
		}
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 24, 12, 0, GL_RGBA, GL_UNSIGNED_BYTE, t);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glClear(GL_COLOR_BUFFER_BIT);
	for (int y = 0; y < 12; y++)
		for (int x = 0; x < 24; x++) {
			float a[4][3];

			for (int k = 0; k < 4; k++) {
				a[k][0] = (x + 0.5f) / 24;
				a[k][1] = (y + 0.5f) / 12;
				a[k][2] = 0;
			}
			quad(4 * x, 4 * y, 4, 4, a);
		}
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	for (int y = 0; y < 12; y++)
		for (int x = 0; x < 24; x++) {
			const uint8_t *q = at(4 * x + 2, 4 * y + 2);

			if (q[0] != 10 * x || q[1] != 20 * y) {
				if (first < 0)
					first = y * 24 + x;
				bad++;
			}
		}
	if (first >= 0) {
		const uint8_t *q = at(4 * (first % 24) + 2, 4 * (first / 24) + 2);

		snprintf(detail, sizeof(detail), "%d texels; the first (%d, %d): %d, %d", bad,
		         first % 24, first / 24, q[0] / 10, q[1] / 20);
	} else {
		detail[0] = 0;
	}
	check("24 x 12, each texel at its middle", !bad, detail);

	/* 48 x 48 one-texel squares, a quad of 24: two texels a pixel, linear */
	for (int i = 0; i < 48 * 48; i++) {
		uint8_t v = ((i % 48) + (i / 48)) & 1 ? 255 : 0;

		t[4 * i] = t[4 * i + 1] = t[4 * i + 2] = v;
		t[4 * i + 3] = 255;
	}
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 48, 48, 0, GL_RGBA, GL_UNSIGNED_BYTE, t);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glClear(GL_COLOR_BUFFER_BIT);
	{
		static const float uv[4][3] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
		int lo = 255, hi = 0;

		quad(0, 0, 24, 24, uv);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		for (int y = 4; y < 20; y++)
			for (int x = 4; x < 20; x++) {
				lo = at(x, y)[0] < lo ? at(x, y)[0] : lo;
				hi = at(x, y)[0] > hi ? at(x, y)[0] : hi;
			}
		snprintf(detail, sizeof(detail), "grey from %d to %d", lo, hi);
		check("48 x 48 minified, linear", lo > 64 && hi < 192, detail);
	}
	glDeleteTextures(1, &tex);

	/* a 6 x 6 cube map: each face's texels told apart */
	{
		static const float major[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
		                                   { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
		static const float sdir[6][3] = { { 0, 0, -1 }, { 0, 0, 1 }, { 1, 0, 0 },
		                                  { 1, 0, 0 }, { 1, 0, 0 }, { -1, 0, 0 } };
		static const float tdir[6][3] = { { 0, -1, 0 }, { 0, -1, 0 }, { 0, 0, 1 },
		                                  { 0, 0, -1 }, { 0, -1, 0 }, { 0, -1, 0 } };

		glUseProgram(pc);
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
		for (int f = 0; f < 6; f++) {
			for (int i = 0; i < 36; i++) {
				t[4 * i] = 40 * f + 20;
				t[4 * i + 1] = (i % 6) * 40 + 20;
				t[4 * i + 2] = (i / 6) * 40 + 20;
				t[4 * i + 3] = 255;
			}
			glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA, 6, 6, 0, GL_RGBA,
			             GL_UNSIGNED_BYTE, t);
		}
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glClear(GL_COLOR_BUFFER_BIT);
		for (int f = 0; f < 6; f++)
			for (int i = 0; i < 36; i++) {
				float sc = ((i % 6) + 0.5f) / 3 - 1, tc = ((i / 6) + 0.5f) / 3 - 1;
				float a[4][3];

				for (int k = 0; k < 3; k++)
					a[0][k] = major[f][k] + sc * sdir[f][k] + tc * tdir[f][k];
				memcpy(a[1], a[0], sizeof(a[0]));
				memcpy(a[2], a[0], sizeof(a[0]));
				memcpy(a[3], a[0], sizeof(a[0]));
				quad(f * 40 + (i % 6) * 6, (i / 6) * 6, 6, 6, a);
			}
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		bad = 0;
		first = -1;
		for (int f = 0; f < 6; f++)
			for (int i = 0; i < 36; i++) {
				const uint8_t *q = at(f * 40 + (i % 6) * 6 + 3, (i / 6) * 6 + 3);

				if (q[0] / 40 != f || q[1] / 40 != i % 6 || q[2] / 40 != i / 6) {
					if (first < 0)
						first = f * 36 + i;
					bad++;
				}
			}
		if (first >= 0) {
			int f = first / 36, i = first % 36;
			const uint8_t *q = at(f * 40 + (i % 6) * 6 + 3, (i / 6) * 6 + 3);

			snprintf(detail, sizeof(detail), "%d texels; the first face %d (%d, %d): %d:%d,%d",
			         bad, f, i % 6, i / 6, q[0] / 40, q[1] / 40, q[2] / 40);
		} else {
			detail[0] = 0;
		}
		check("a 6 x 6 cube map, each texel", !bad, detail);
		glDeleteTextures(1, &tex);
	}
}

/* --probe: with SGX_TEX_PROBE=1 (the driver's copy holding each texel's
 * index), where the sampler reads each face's levels of an n x n cube map
 * with every level: the least index a quad over the face at that level
 * brings back */
static void
probe(int n)
{
	static const char *vs = "attribute vec2 p; attribute vec3 a; varying vec3 d;\n"
	                        "void main() { d = a; gl_Position = vec4(p, 0.0, 1.0); }\n";
	static const char *fs = "precision mediump float; varying vec3 d; uniform samplerCube c;\n"
	                        "void main() { gl_FragColor = textureCube(c, d); }\n";
	static const float major[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
	                                   { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
	static const float corner[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
	GLuint p = program(vs, fs), tex;
	int levels = 0;
	uint8_t *t = calloc(n * n, 4);

	/* (a size not a power of two: one level, GLES 2's rule) */
	while ((n >> levels) > 0 && !(n & (n - 1)))
		levels++;
	levels += !levels;
	glUseProgram(p);
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
	for (int f = 0; f < 6; f++)
		for (int l = 0; l < levels; l++)
			glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, l, GL_RGBA, n >> l, n >> l, 0,
			             GL_RGBA, GL_UNSIGNED_BYTE, t);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER,
	                levels > 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	printf("%d x %d, %d levels: each face's levels' least index (texels)\n", n, n, levels);
	for (int f = 0; f < 6; f++) {
		printf("  face %d:", f);
		for (int l = 0; l < levels; l++) {
			int side = n >> l, ax = major[f][0] != 0 ? 0 : major[f][1] != 0 ? 1 : 2;
			unsigned least = ~0u;
			float a[4][3];

			for (int k = 0; k < 4; k++) {
				a[k][ax] = major[f][ax];
				a[k][(ax + 1) % 3] = corner[k][0];
				a[k][(ax + 2) % 3] = corner[k][1];
			}
			glClear(GL_COLOR_BUFFER_BIT);
			quad(0, 0, side, side, a);
			glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
			for (int y = 0; y < side; y++)
				for (int x = 0; x < side; x++) {
					const uint8_t *q = at(x, y);
					unsigned k = q[0] | q[1] << 8 | q[2] << 16;

					if (k < least)
						least = k;
				}
			printf(" %u", least);
		}
		printf("\n");
	}
	free(t);
	glDeleteTextures(1, &tex);
}

int main(int argc, char **argv)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	GLuint rt, fbo;
	EGLDisplay dpy;
	EGLContext ctx;

	dump = argc > 1 && !strcmp(argv[1], "--dump");
	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glmip: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glmip: no GLES 2 context\n"), 2;
	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glViewport(0, 0, W, H);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glActiveTexture(GL_TEXTURE0);

	if (argc > 2 && !strcmp(argv[1], "--probe")) {
		for (int i = 2; i < argc; i++)
			probe(atoi(argv[i]));
		return 0;
	}
	mipmaps();
	cube();
	cube_levels();
	npot();

	printf("%d of %d cases right\n", cases - failed, cases);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return failed ? 1 : 0;
}
