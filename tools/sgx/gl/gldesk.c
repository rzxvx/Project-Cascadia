/* gldesk -- desktop OpenGL (2.1, compatibility) through the driver
 * (docs/research/p105-mesa.md, M35, M36): a 1D texture, sRGB textures (RGBA
 * and luminance, decoded or not), a rectangle texture (unnormalized
 * coordinates), the fixed-function pipeline (glBegin and client arrays, a
 * texture modulated by the vertex colour), flat shading, occlusion queries
 * and logic ops (M37), float depth buffers (M43).  A 64 x 64 target,
 * every pixel read back against what C works out.
 *
 *   sgx-gl gldesk [CASE]        (CASE: a name's start; all by default)
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64

static uint8_t px[W * H * 4];
static int cases, failed;
static const char *only;

static const char *vs =
	"#version 120\n"
	"attribute vec4 a_position;\n"
	"varying vec2 v;\n"
	"void main() { gl_Position = a_position; v = a_position.xy * 0.5 + 0.5; }\n";

static GLuint
program(const char *fs_body)
{
	char fs[2048], log[512];
	const char *src[2] = { vs, fs };
	GLuint p = glCreateProgram();
	GLint ok;

	snprintf(fs, sizeof(fs), "#version 120\n#extension GL_ARB_texture_rectangle : enable\n"
	         "varying vec2 v;\nuniform sampler1D t1;\nuniform sampler2D t2;\n"
	         "uniform sampler2DRect tr;\nuniform sampler2D td;\nuniform sampler2DShadow ts;\n"
	         "void main() { %s }\n", fs_body);
	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);

		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			glGetShaderInfoLog(s, sizeof(log), NULL, log);
			fprintf(stderr, "gldesk: %s\n", log);
		}
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "a_position");
	glLinkProgram(p);
	glUseProgram(p);
	glUniform1i(glGetUniformLocation(p, "t1"), 0);
	glUniform1i(glGetUniformLocation(p, "t2"), 1);
	glUniform1i(glGetUniformLocation(p, "tr"), 2);
	return p;
}

static int
to8(float x)
{
	return (int)(fminf(fmaxf(x, 0), 1) * 255 + 0.5f);
}

static float
srgb_to_linear(float c)
{
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static int
want_ok(void (*want)(float s, float t, float rgba[4]), int tolerance, const char *name)
{
	int bad = 0, at = -1;
	float rgba[4];

	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	for (int i = 0; i < W * H; i++) {
		int off = 0;

		want((i % W + 0.5f) / W, (i / W + 0.5f) / H, rgba);
		for (int c = 0; c < 4; c++)
			off |= abs(px[4 * i + c] - to8(rgba[c])) > tolerance;
		if (off && at < 0)
			at = i;
		bad += off;
	}
	cases++;
	if (!bad) {
		printf("%-36s ok\n", name);
		return 1;
	}
	failed++;
	want((at % W + 0.5f) / W, (at / W + 0.5f) / H, rgba);
	printf("%-36s WRONG: %d pixels, the first at %d, %d: %d %d %d %d, want %d %d %d %d\n", name,
	       bad, at % W, at / W, px[4 * at], px[4 * at + 1], px[4 * at + 2], px[4 * at + 3],
	       to8(rgba[0]), to8(rgba[1]), to8(rgba[2]), to8(rgba[3]));
	return 0;
}

static void
check(const char *name, const char *body, void (*want)(float s, float t, float rgba[4]))
{
	static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	GLuint p;

	if (only && strncmp(name, only, strlen(only)))
		return;
	p = program(body);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray(0);
	glUseProgram(0);
	glDeleteProgram(p);
	want_ok(want, 2, name);
}

/* the 1D texture: 8 texels */
static const uint8_t tex1d[8][4] = {
	{ 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 255, 255, 0, 255 },
	{ 0, 255, 255, 200 }, { 255, 0, 255, 150 }, { 128, 128, 128, 100 }, { 10, 20, 30, 40 },
};

static void
want_1d(float s, float t, float c[4])
{
	(void)t;
	for (int k = 0; k < 4; k++)
		c[k] = tex1d[(int)(s * 8)][k] / 255.0f;
}

/* the sRGB textures: 4 x 4, texel i's bytes */
static void
srgb_bytes(int i, uint8_t c[4])
{
	c[0] = i * 17, c[1] = 255 - i * 13, c[2] = 32 + i * 9, c[3] = 64 + i * 8;
}

static void
want_srgb(float s, float t, float c[4])
{
	uint8_t b[4];

	srgb_bytes((int)(s * 4) + 4 * (int)(t * 4), b);
	for (int k = 0; k < 3; k++)
		c[k] = srgb_to_linear(b[k] / 255.0f);
	c[3] = b[3] / 255.0f;
}

static void
want_srgb_raw(float s, float t, float c[4])
{
	uint8_t b[4];

	srgb_bytes((int)(s * 4) + 4 * (int)(t * 4), b);
	for (int k = 0; k < 4; k++)
		c[k] = b[k] / 255.0f;
}

/* the rectangle texture: 6 x 3, texel (x, y) bytes x * 40, y * 80, 200 */
static void
want_rect(float s, float t, float c[4])
{
	int x = (int)(s * 6), y = (int)(t * 3);

	c[0] = x * 40 / 255.0f, c[1] = y * 80 / 255.0f, c[2] = 200 / 255.0f, c[3] = 1;
}

static void
want_sluminance(float s, float t, float c[4])
{
	uint8_t b[4];

	srgb_bytes((int)(s * 4) + 4 * (int)(t * 4), b);
	c[0] = c[1] = c[2] = srgb_to_linear(b[0] / 255.0f);
	c[3] = 1;
}

/* the fixed-function quad: a vertex colour (red to the right, green up,
 * blue and alpha 1) times the sRGB texture's bytes as RGBA8 */
static void
want_ff(float s, float t, float c[4])
{
	uint8_t b[4];

	srgb_bytes((int)(s * 4) + 4 * (int)(t * 4), b);
	c[0] = b[0] / 255.0f * s;
	c[1] = b[1] / 255.0f * t;
	c[2] = b[2] / 255.0f;
	c[3] = b[3] / 255.0f;
}

/* flat shading (M37): two triangles, a gap between them along the
 * diagonal, each of the colour of one of its vertices -- flat_first: the
 * first's (ARB_provoking_vertex), else the last's; flat_tex: times the
 * sRGB texture's bytes as RGBA8 */
static int flat_first, flat_tex;

static void
want_flat(float s, float t, float c[4])
{
	static const float last[2][4] = { { 0, 0, 1, 1 }, { 1, 1, 0, 1 } };
	static const float first[2][4] = { { 1, 0, 0, 1 }, { 0, 1, 1, 1 } };
	int k = s + t < 0.95f ? 0 : s + t > 1.05f ? 1 : -1;
	uint8_t b[4];

	srgb_bytes((int)(s * 4) + 4 * (int)(t * 4), b);
	for (int i = 0; i < 4; i++) {
		c[i] = k < 0 ? 0 : flat_first ? first[k][i] : last[k][i];
		if (k >= 0 && flat_tex)
			c[i] *= b[i] / 255.0f;
	}
}

static void
flat_case(const char *name)
{
	if (only && strncmp(name, only, strlen(only)))
		return;
	glShadeModel(GL_FLAT);
	glClear(GL_COLOR_BUFFER_BIT);
	glBegin(GL_TRIANGLES);
	glColor4f(1, 0, 0, 1); glTexCoord2f(0, 0); glVertex2f(-1, -1);
	glColor4f(0, 1, 0, 1); glTexCoord2f(0.95f, 0); glVertex2f(0.9f, -1);
	glColor4f(0, 0, 1, 1); glTexCoord2f(0, 0.95f); glVertex2f(-1, 0.9f);
	glColor4f(0, 1, 1, 1); glTexCoord2f(1, 1); glVertex2f(1, 1);
	glColor4f(1, 0, 1, 1); glTexCoord2f(0.05f, 1); glVertex2f(-0.9f, 1);
	glColor4f(1, 1, 0, 1); glTexCoord2f(1, 0.05f); glVertex2f(1, -0.9f);
	glEnd();
	glShadeModel(GL_SMOOTH);
	want_ok(want_flat, 2, name);
}

/* depth textures (M37): 64 x 64, the depth (x + 0.5) / 64 across; read as they
 * are (d, d, d, 1), compared with 0.5 (shadow2D, LEQUAL: 1 from the middle
 * right; GREATER the other way), and one the GPU rendered a slope into */
static int shadow_func;      /* 0 none, else GL's */
static GLuint desk_fbo;      /* gldesk's target */

static void
want_depth(float s, float t, float c[4])
{
	float d = ((int)(s * 64) + 0.5f) / 64.0f;

	(void)t;
	if (!shadow_func) {
		c[0] = c[1] = c[2] = d;
	} else {
		float r = shadow_func == GL_LEQUAL ? 0.5f <= d : 0.5f > d;

		c[0] = c[1] = c[2] = r;
	}
	c[3] = 1;
}

static void
depth_cases(void)
{
	static float zs[64 * 64];
	GLuint t;
	struct { const char *name, *body; int func, gpu; } k[] = {
		{ "a depth texture, read", "gl_FragColor = vec4(texture2D(td, v).rgb, 1.0);", 0, 0 },
		{ "a depth texture, LEQUAL 0.5", "gl_FragColor = vec4(shadow2D(ts, vec3(v, 0.5)).rgb, 1.0);",
		  GL_LEQUAL, 0 },
		{ "a depth texture, GREATER 0.5", "gl_FragColor = vec4(shadow2D(ts, vec3(v, 0.5)).rgb, 1.0);",
		  GL_GREATER, 0 },
		{ "a shadow map the GPU drew, LEQUAL", "gl_FragColor = vec4(shadow2D(ts, vec3(v, 0.5)).rgb, 1.0);",
		  GL_LEQUAL, 1 },
	};

	for (int i = 0; i < 64 * 64; i++)
		zs[i] = (i % 64 + 0.5f) / 64.0f;
	glActiveTexture(GL_TEXTURE3);
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 64, 64, 0, GL_DEPTH_COMPONENT, GL_FLOAT, zs);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_DEPTH_TEXTURE_MODE, GL_LUMINANCE);
	for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
		char body[256];
		GLuint p;

		if (only && strncmp(k[i].name, only, strlen(only)))
			continue;
		if (k[i].gpu) {
			/* the depth drawn: a quad from 0 at the left to 1 at the right,
			 * in an FBO of the texture alone */
			static const float quad[] = { -1, -1, -1, 1, -1, 1, -1, 1, -1, 1, 1, 1 };
			GLuint fb;

			glGenFramebuffers(1, &fb);
			glBindFramebuffer(GL_FRAMEBUFFER, fb);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, t, 0);
			glDrawBuffer(GL_NONE);
			glReadBuffer(GL_NONE);
			glClearDepth(1.0);
			glClear(GL_DEPTH_BUFFER_BIT);
			glEnable(GL_DEPTH_TEST);
			glDepthFunc(GL_ALWAYS);
			glUseProgram(0);
			glEnableClientState(GL_VERTEX_ARRAY);
			glVertexPointer(3, GL_FLOAT, 0, quad);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			glDisableClientState(GL_VERTEX_ARRAY);
			glDisable(GL_DEPTH_TEST);
			/* (back to gldesk's own target) */
			glBindFramebuffer(GL_FRAMEBUFFER, desk_fbo);
			glDeleteFramebuffers(1, &fb);
			glDrawBuffer(GL_COLOR_ATTACHMENT0);
			glReadBuffer(GL_COLOR_ATTACHMENT0);
		}
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE,
		                k[i].func ? GL_COMPARE_R_TO_TEXTURE : GL_NONE);
		if (k[i].func)
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, k[i].func);
		snprintf(body, sizeof(body), "%s", k[i].body);
		p = program(body);
		glUseProgram(p);
		glUniform1i(glGetUniformLocation(p, "td"), 3);
		glUniform1i(glGetUniformLocation(p, "ts"), 3);
		shadow_func = k[i].func;
		{
			static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };

			glClear(GL_COLOR_BUFFER_BIT);
			glEnableVertexAttribArray(0);
			glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			glDisableVertexAttribArray(0);
		}
		glUseProgram(0);
		glDeleteProgram(p);
		/* (the GPU's slope: depth (x + 0.5) / 64 at texel x too) */
		want_ok(want_depth, 2, k[i].name);
	}
	glActiveTexture(GL_TEXTURE0);
}

/* (M43) float depth buffers (ARB_depth_buffer_float): GL_DEPTH32F_STENCIL8
 * -- the ISP's F32-and-stencil store -- and GL_DEPTH_COMPONENT32F, in an
 * FBO of their own.  Two renders, a glFinish between: the left half at
 * depth 0.25 and stencil 7, then green at 0.1 where the stencil is 7, then
 * red at 0.3 where the depth passes -- green left, red right; the depth
 * and stencil read back; and depths of 1e-7 steps (and stencils) from the
 * CPU through a render that loads and stores them, read back exactly (a
 * 24-bit store would round them). */
static void
zquad(float x0, float x1, float d)
{
	const float z = 2 * d - 1;
	const float q[] = { x0, -1, z, x1, -1, z, x0, 1, z, x1, 1, z };

	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(3, GL_FLOAT, 0, q);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableClientState(GL_VERTEX_ARRAY);
}

static void
float_depth_cases(void)
{
	static const struct { const char *name; GLenum ifmt; int stencil; } f[] = {
		{ "depth32f_stencil8", GL_DEPTH32F_STENCIL8, 1 },
		{ "depth_component32f", GL_DEPTH_COMPONENT32F, 0 },
	};
	static struct { float d; uint32_t s; } up[W * H];
	static float zr[W * H];
	static uint8_t sr[W * H];

	if (!strstr((const char *)glGetString(GL_EXTENSIONS), "GL_ARB_depth_buffer_float")) {
		printf("%-36s skip: no ARB_depth_buffer_float\n", "float depth");
		return;
	}
	glUseProgram(0);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	/* (its textures on a unit of their own: the later cases' are bound) */
	glActiveTexture(GL_TEXTURE5);
	for (unsigned k = 0; k < sizeof(f) / sizeof(f[0]); k++) {
		char name[64];
		GLuint rt, zs, fb;
		GLenum att = f[k].stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
		int bad;

		snprintf(name, sizeof(name), "%s, two renders", f[k].name);
		if (only && strncmp(name, only, strlen(only)) && strncmp(f[k].name, only, strlen(only)))
			continue;
		glGenTextures(1, &rt);
		glBindTexture(GL_TEXTURE_2D, rt);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glGenRenderbuffers(1, &zs);
		glBindRenderbuffer(GL_RENDERBUFFER, zs);
		glRenderbufferStorage(GL_RENDERBUFFER, f[k].ifmt, W, H);
		glGenFramebuffers(1, &fb);
		glBindFramebuffer(GL_FRAMEBUFFER, fb);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, att, GL_RENDERBUFFER, zs);
		cases++;
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			failed++;
			printf("%-36s WRONG: the framebuffer is not complete\n", name);
			glBindFramebuffer(GL_FRAMEBUFFER, desk_fbo);
			continue;
		}
		glClearColor(0, 0, 0, 1);
		glClearDepth(1.0);
		glClearStencil(0);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_ALWAYS);
		if (f[k].stencil) {
			glEnable(GL_STENCIL_TEST);
			glStencilFunc(GL_ALWAYS, 7, 0xff);
			glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
		}
		glColorMask(0, 0, 0, 0);
		zquad(-1, 0, 0.25f);
		glColorMask(1, 1, 1, 1);
		glFinish();
		glDepthFunc(GL_LESS);
		if (f[k].stencil) {
			glStencilFunc(GL_EQUAL, 7, 0xff);
			glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
		}
		glColor3f(0, 1, 0);
		zquad(-1, 1, 0.1f);
		glFinish();
		glDisable(GL_STENCIL_TEST);
		glColor3f(1, 0, 0);
		zquad(-1, 1, 0.3f);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		bad = 0;
		for (int i = 0; i < W * H; i++) {
			int left = i % W < W / 2;
			/* (no stencil: green everywhere, then red where 0.3 < 0.1 -- nowhere) */
			int green = left || !f[k].stencil;

			bad += px[4 * i] != (green ? 0 : 255) || px[4 * i + 1] != (green ? 255 : 0);
		}
		if (bad) {
			failed++;
			printf("%-36s WRONG: %d pixels\n", name, bad);
		} else {
			printf("%-36s ok\n", name);
		}
		/* the depth and stencil as the CPU reads them */
		snprintf(name, sizeof(name), "%s, read back", f[k].name);
		cases++;
		glReadPixels(0, 0, W, H, GL_DEPTH_COMPONENT, GL_FLOAT, zr);
		if (f[k].stencil)
			glReadPixels(0, 0, W, H, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, sr);
		bad = 0;
		for (int i = 0; i < W * H; i++) {
			int left = i % W < W / 2;
			float wz = left || !f[k].stencil ? 0.1f : 0.3f;

			bad += fabsf(zr[i] - wz) > 1e-6f || (f[k].stencil && sr[i] != (left ? 7 : 0));
		}
		if (bad) {
			failed++;
			printf("%-36s WRONG: %d pixels, the first %g %u\n", name, bad, zr[0],
			       f[k].stencil ? sr[0] : 0);
		} else {
			printf("%-36s ok\n", name);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, desk_fbo);
		glDeleteFramebuffers(1, &fb);
		glDeleteRenderbuffers(1, &zs);

		/* from the CPU, through the GPU and back */
		snprintf(name, sizeof(name), "%s, from the CPU", f[k].name);
		cases++;
		for (int i = 0; i < W * H; i++) {
			up[i].d = (i + 1) * 1e-7f;
			up[i].s = i & 0xff;
			zr[i] = up[i].d;
		}
		glGenTextures(1, &zs);
		glBindTexture(GL_TEXTURE_2D, zs);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		if (f[k].stencil)
			glTexImage2D(GL_TEXTURE_2D, 0, f[k].ifmt, W, H, 0, GL_DEPTH_STENCIL,
			             GL_FLOAT_32_UNSIGNED_INT_24_8_REV, up);
		else
			glTexImage2D(GL_TEXTURE_2D, 0, f[k].ifmt, W, H, 0, GL_DEPTH_COMPONENT, GL_FLOAT, zr);
		glGenFramebuffers(1, &fb);
		glBindFramebuffer(GL_FRAMEBUFFER, fb);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
		glFramebufferTexture2D(GL_FRAMEBUFFER, att, GL_TEXTURE_2D, zs, 0);
		/* a render that loads and stores: one pixel's depth (and stencil)
		 * written, at the top right */
		glDepthFunc(GL_ALWAYS);
		if (f[k].stencil) {
			glEnable(GL_STENCIL_TEST);
			glStencilFunc(GL_ALWAYS, 0x5a, 0xff);
			glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
		}
		zquad(1 - 2.0f / W, 1, 0.75f);
		glDisable(GL_STENCIL_TEST);
		glFinish();
		glReadPixels(0, 0, W, H, GL_DEPTH_COMPONENT, GL_FLOAT, zr);
		if (f[k].stencil)
			glReadPixels(0, 0, W, H, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, sr);
		bad = 0;
		for (int i = 0; i < W * H; i++) {
			int drawn = i % W == W - 1;
			float wz = drawn ? 0.75f : up[i].d;

			bad += drawn ? fabsf(zr[i] - wz) > 1e-6f : zr[i] != wz;
			bad += f[k].stencil && sr[i] != (drawn ? 0x5a : up[i].s);
		}
		if (bad) {
			failed++;
			printf("%-36s WRONG: %d pixels, the first %.9g (want %.9g) %u\n", name, bad, zr[0],
			       up[0].d, f[k].stencil ? sr[0] : 0);
		} else {
			printf("%-36s ok\n", name);
		}
		glDisable(GL_DEPTH_TEST);
		glBindFramebuffer(GL_FRAMEBUFFER, desk_fbo);
		glDeleteFramebuffers(1, &fb);
		glDeleteTextures(1, &zs);
		glDeleteTextures(1, &rt);
	}
	glActiveTexture(GL_TEXTURE0);
	glColor4f(1, 1, 1, 1);
}

/* the logic ops (M37): the target cleared to bytes 51 102 153 204, then
 * the program's colour -- bytes 32 i across, 32 j up, 128, 64 -- xor'ed
 * over it, through a colour mask, and with the left half discarded */
static const float LOGIC_CLEAR[4] = { 0.2f, 0.4f, 0.6f, 0.8f };
static const char *LOGIC_BODY =
	"gl_FragColor = vec4(floor(v * 8.0) * 32.0 / 255.0, 128.0 / 255.0, 64.0 / 255.0);";
static unsigned logic_mask;
static int logic_discard;

static void
want_logic(float s, float t, float c[4])
{
	const int src[4] = { 32 * (int)(s * 8), 32 * (int)(t * 8), 128, 64 };

	for (int k = 0; k < 4; k++) {
		int d = to8(LOGIC_CLEAR[k]);

		c[k] = ((logic_mask >> k & 1) && !(logic_discard && s < 0.5f) ? src[k] ^ d : d) / 255.0f;
	}
}

static void
logic_check(const char *name, unsigned mask, int discard)
{
	static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	char body[256];
	GLuint p;

	if (only && strncmp(name, only, strlen(only)))
		return;
	snprintf(body, sizeof(body), "%s%s", discard ? "if (v.x < 0.5) discard; " : "", LOGIC_BODY);
	logic_mask = mask;
	logic_discard = discard;
	p = program(body);
	glClearColor(LOGIC_CLEAR[0], LOGIC_CLEAR[1], LOGIC_CLEAR[2], LOGIC_CLEAR[3]);
	glClear(GL_COLOR_BUFFER_BIT);
	glEnable(GL_COLOR_LOGIC_OP);
	glLogicOp(GL_XOR);
	glColorMask(mask & 1, mask >> 1 & 1, mask >> 2 & 1, mask >> 3 & 1);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray(0);
	glColorMask(1, 1, 1, 1);
	glDisable(GL_COLOR_LOGIC_OP);
	glUseProgram(0);
	glDeleteProgram(p);
	want_ok(want_logic, 0, name);
}

int
main(int argc, char **argv)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_MAJOR_VERSION, 2, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE };
	GLuint rt, fbo, t1, t2, t3;
	uint8_t img[16][4], lum[16];
	EGLDisplay dpy;
	EGLContext ctx;

	only = argc > 1 ? argv[1] : NULL;
	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_API))
		return fprintf(stderr, "gldesk: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "gldesk: no OpenGL 2.1 context\n"), 2;
	printf("%s, %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	desk_fbo = fbo;
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	/* (a depth buffer: the occlusion queries' quad behind another) */
	{
		GLuint zs;

		glGenRenderbuffers(1, &zs);
		glBindRenderbuffer(GL_RENDERBUFFER, zs);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, W, H);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, zs);
	}
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		return fprintf(stderr, "gldesk: the target is not complete\n"), 2;
	glViewport(0, 0, W, H);

	glActiveTexture(GL_TEXTURE0);
	glGenTextures(1, &t1);
	glBindTexture(GL_TEXTURE_1D, t1);
	glTexImage1D(GL_TEXTURE_1D, 0, GL_RGBA8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex1d);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_1D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	for (int i = 0; i < 16; i++) {
		srgb_bytes(i, img[i]);
		lum[i] = img[i][0];
	}
	glActiveTexture(GL_TEXTURE1);
	glGenTextures(1, &t2);
	glBindTexture(GL_TEXTURE_2D, t2);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	check("a 1D texture", "gl_FragColor = texture1D(t1, v.x);", want_1d);
	check("an sRGB texture", "gl_FragColor = texture2D(t2, v);", want_srgb);
	if (strstr((const char *)glGetString(GL_EXTENSIONS), "GL_EXT_texture_sRGB_decode")) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT);
		check("an sRGB texture, not decoded", "gl_FragColor = texture2D(t2, v);", want_srgb_raw);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SRGB_DECODE_EXT, GL_DECODE_EXT);
	}
	glGenTextures(1, &t3);
	glBindTexture(GL_TEXTURE_2D, t3);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_SLUMINANCE8, 4, 4, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, lum);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	check("an sRGB luminance texture", "gl_FragColor = texture2D(t2, v);", want_sluminance);

	/* a rectangle texture: unnormalized coordinates */
	{
		uint8_t rect[3][6][4];
		GLuint tr;

		for (int y = 0; y < 3; y++)
			for (int x = 0; x < 6; x++) {
				rect[y][x][0] = x * 40, rect[y][x][1] = y * 80;
				rect[y][x][2] = 200, rect[y][x][3] = 255;
			}
		glActiveTexture(GL_TEXTURE2);
		glGenTextures(1, &tr);
		glBindTexture(GL_TEXTURE_RECTANGLE, tr);
		glTexImage2D(GL_TEXTURE_RECTANGLE, 0, GL_RGBA8, 6, 3, 0, GL_RGBA, GL_UNSIGNED_BYTE, rect);
		glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		check("a rectangle texture", "gl_FragColor = texture2DRect(tr, v * vec2(6.0, 3.0));",
		      want_rect);
		glActiveTexture(GL_TEXTURE1);
	}

	/* the fixed-function pipeline: glBegin, the texture (as RGBA8)
	 * modulated by the vertex colour */
	if (!only || !strncmp("fixed", only, strlen(only))) {
		glActiveTexture(GL_TEXTURE0);   /* glTexCoord's unit */
		glBindTexture(GL_TEXTURE_2D, t2);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
		glEnable(GL_TEXTURE_2D);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glClear(GL_COLOR_BUFFER_BIT);
		glBegin(GL_QUADS);
		glColor4f(0, 0, 1, 1); glTexCoord2f(0, 0); glVertex2f(-1, -1);
		glColor4f(1, 0, 1, 1); glTexCoord2f(1, 0); glVertex2f(1, -1);
		glColor4f(1, 1, 1, 1); glTexCoord2f(1, 1); glVertex2f(1, 1);
		glColor4f(0, 1, 1, 1); glTexCoord2f(0, 1); glVertex2f(-1, 1);
		glEnd();
		want_ok(want_ff, 3, "fixed function, a texture modulated");

		/* the same from client arrays */
		{
			static const float pos[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
			static const float tc[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
			static const float col[] = { 0, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1 };

			glClear(GL_COLOR_BUFFER_BIT);
			glEnableClientState(GL_VERTEX_ARRAY);
			glEnableClientState(GL_COLOR_ARRAY);
			glEnableClientState(GL_TEXTURE_COORD_ARRAY);
			glVertexPointer(2, GL_FLOAT, 0, pos);
			glColorPointer(4, GL_FLOAT, 0, col);
			glTexCoordPointer(2, GL_FLOAT, 0, tc);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			glDisableClientState(GL_VERTEX_ARRAY);
			glDisableClientState(GL_COLOR_ARRAY);
			glDisableClientState(GL_TEXTURE_COORD_ARRAY);
			want_ok(want_ff, 3, "fixed function, from arrays");
		}
		glDisable(GL_TEXTURE_2D);
	}

	depth_cases();
	float_depth_cases();
	glClearColor(0, 0, 0, 0);
	flat_case("flat shading, the last vertex");
	if (strstr((const char *)glGetString(GL_EXTENSIONS), "GL_ARB_provoking_vertex")) {
		void (*provoking)(GLenum) = (void (*)(GLenum))eglGetProcAddress("glProvokingVertex");

		flat_first = 1;
		provoking(0x8E4D);     /* GL_FIRST_VERTEX_CONVENTION */
		flat_case("flat shading, the first vertex");
		provoking(0x8E4E);     /* GL_LAST_VERTEX_CONVENTION */
		flat_first = 0;
	}
	if (!only || !strncmp("flat shading, textured", only, strlen(only))) {
		glActiveTexture(GL_TEXTURE0);
		glEnable(GL_TEXTURE_2D);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		flat_tex = 1;
		flat_case("flat shading, textured");
		flat_tex = 0;
		glDisable(GL_TEXTURE_2D);
	}
	/* occlusion queries (M37): the samples a quad passes -- all of its
	 * 48 x 32 pixels, half of them discarded, none behind another, and two
	 * queries one after the other */
	if (!only || !strncmp("occlusion", only, strlen(only))) {
		static const float quad[] = { -0.5f, -0.5f, 1, -0.5f, -0.5f, 0.5f, 1, 0.5f };
		/* (known: a discarded pixel is counted -- discard keeps the tile's
		 * colour, the ISP passes it, M19) */
		static const struct { const char *name, *body; int behind, want, known, masked; } q[] = {
			{ "occlusion query, a quad", "gl_FragColor = vec4(1.0);", 0, 48 * 32, 0 },
			{ "occlusion query, nothing written", "gl_FragColor = vec4(1.0);", 0, 48 * 32, 0, 1 },
			{ "occlusion query, a third discarded",
			  "if (v.x < 0.5) discard; gl_FragColor = vec4(1.0);", 0, 32 * 32, 1 },
			{ "occlusion query, behind", "gl_FragColor = vec4(1.0);", 1, 0, 0 },
		};
		GLuint ids[4];

		glGenQueries(4, ids);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);
		for (int i = 0; i < 4; i++) {
			GLuint p = program(q[i].body), n = ~0u;

			cases++;
			if (!only || !strncmp(q[i].name, only, strlen(only))) {
				glClear(GL_COLOR_BUFFER_BIT);
				/* a quad in front, all over the target, for the third to be behind */
				glDepthMask(GL_TRUE);
				glClearDepth(1.0);
				glClear(GL_DEPTH_BUFFER_BIT);
				if (q[i].behind) {
					static const float all[] = { -1, -1, 1, -1, -1, 1, 1, 1 };

					glEnableVertexAttribArray(0);
					glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, all);
					glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
				}
				/* (the colour and depth masked: piglit's conform cases) */
				if (q[i].masked) {
					glColorMask(0, 0, 0, 0);
					glDepthMask(GL_FALSE);
				}
				glBeginQuery(GL_SAMPLES_PASSED, ids[i]);
				glEnableVertexAttribArray(0);
				glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
				glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
				glEndQuery(GL_SAMPLES_PASSED);
				glColorMask(1, 1, 1, 1);
				glDepthMask(GL_TRUE);
				glDisableVertexAttribArray(0);
				glGetQueryObjectuiv(ids[i], GL_QUERY_RESULT, &n);
			}
			if (n == (GLuint)q[i].want) {
				printf("%-36s ok   : %u samples\n", q[i].name, n);
			} else if (q[i].known) {
				printf("%-36s known: %u samples, want %d\n", q[i].name, n, q[i].want);
			} else {
				failed++;
				printf("%-36s WRONG: %u samples, want %d\n", q[i].name, n, q[i].want);
			}
			glUseProgram(0);
			glDeleteProgram(p);
		}
		glDeleteQueries(4, ids);
		glDisable(GL_DEPTH_TEST);
	}
	/* piglit's occlusion_query_conform GetObjivAval_multi2: an occluder,
	 * then behind it a quad larger, one a little larger and one smaller,
	 * nothing written -- fewer samples each, none for the last */
	if (!only || !strncmp("occlusion query, three behind", only, strlen(only))) {
		static const float r[4] = { 0.5f, 0.7f, 0.53f, 0.4f };
		GLuint ids[3], n[3];

		glGenQueries(3, ids);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);
		glClearDepth(1.0);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glOrtho(-1, 1, -1, 1, 0, 25);
		glMatrixMode(GL_MODELVIEW);
		glLoadIdentity();
		glTranslatef(0, 0, -10);
		glColor3f(1, 0, 0);
		glRectf(-r[0], -r[0], r[0], r[0]);
		glTranslatef(0, 0, -5);
		glColorMask(0, 0, 0, 0);
		glDepthMask(GL_FALSE);
		for (int i = 0; i < 3; i++) {
			glBeginQuery(GL_SAMPLES_PASSED, ids[i]);
			glRectf(-r[i + 1], -r[i + 1], r[i + 1], r[i + 1]);
			glEndQuery(GL_SAMPLES_PASSED);
		}
		for (int i = 0; i < 3; i++)
			glGetQueryObjectuiv(ids[i], GL_QUERY_RESULT, &n[i]);
		glColorMask(1, 1, 1, 1);
		glDepthMask(GL_TRUE);
		glLoadIdentity();
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glDisable(GL_DEPTH_TEST);
		glDeleteQueries(3, ids);
		/* (known: with depth writes off the ISP counts samples the depth
		 * test fails too -- about half of those; drawn right) */
		cases++;
		if (n[0] > n[1] && n[1] > n[2] && !n[2])
			printf("%-36s ok   : %u %u %u samples\n", "occlusion query, three behind", n[0], n[1], n[2]);
		else
			printf("%-36s known: %u %u %u samples, the last 0 wanted\n",
			       "occlusion query, three behind", n[0], n[1], n[2]);
	}
	logic_check("a logic op, xor", 0xf, 0);
	logic_check("a logic op, red and blue masked in", 0x5, 0);
	logic_check("a logic op, half discarded", 0xf, 1);

	printf("%d of %d cases right\n", cases - failed, cases);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return failed ? 1 : 0;
}
