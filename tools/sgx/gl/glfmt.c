/* glfmt -- texture formats beyond 8-bit RGBA through the driver (docs/
 * research/p105-mesa.md, M43): float and half float textures
 * (ARB_texture_float), signed (EXT_texture_snorm), 16-bit and 10-bit
 * channels, the shared exponent format (EXT_texture_shared_exponent).  A
 * 2 x 2 texture each, its texels sampled at their centres (and between
 * them, filtered), brought into [0, 1] by the program -- value * scale +
 * bias -- so that values past [0, 1] can be seen on an RGBA8 target, and
 * read back against what GL says the format holds.  A format the context
 * has not is said, not counted.  And render targets of other formats than
 * RGBA8 (R8, RG8: ARB_texture_rg): cleared, then -- a render later, its
 * tiles loaded back from memory -- half drawn over, then added to with
 * blending, read back and sampled.
 *
 *   sgx-gl glfmt [CASE]        (CASE: a name's start; all by default)
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
static int cases, failed, skipped;
static const char *only;
static GLuint prog;

/* how a format keeps a value: what GL hands back for it */
enum keep { F32, F16, SNORM8, UNORM16, UNORM10_2, E5 };

struct fmt_case {
	const char *name;
	GLenum ifmt, fmt;
	enum keep keep;
	int channels;        /* the internal format's: 1 L or A or I by kind, 3 RGB, 4 RGBA */
	char kind;           /* 'c' colour, 'l' luminance, 'a' alpha, 'L' luminance alpha, 'i' intensity */
};

static const struct fmt_case fmts[] = {
	{ "rgba32f", GL_RGBA32F_ARB, GL_RGBA, F32, 4, 'c' },
	{ "rgba16f", GL_RGBA16F_ARB, GL_RGBA, F16, 4, 'c' },
	{ "rgb32f", GL_RGB32F_ARB, GL_RGB, F32, 3, 'c' },
	{ "rgb16f", GL_RGB16F_ARB, GL_RGB, F16, 3, 'c' },
	{ "alpha16f", GL_ALPHA16F_ARB, GL_RGBA, F16, 1, 'a' },
	{ "luminance32f", GL_LUMINANCE32F_ARB, GL_RGBA, F32, 1, 'l' },
	{ "lum-alpha16f", GL_LUMINANCE_ALPHA16F_ARB, GL_RGBA, F16, 2, 'L' },
	{ "intensity16f", GL_INTENSITY16F_ARB, GL_RGBA, F16, 1, 'i' },
	{ "rgba8-snorm", GL_RGBA8_SNORM, GL_RGBA, SNORM8, 4, 'c' },
	{ "rgba16", GL_RGBA16, GL_RGBA, UNORM16, 4, 'c' },
	{ "rgb10-a2", GL_RGB10_A2, GL_RGBA, UNORM10_2, 4, 'c' },
	{ "rgb9-e5", GL_RGB9_E5_EXT, GL_RGB, E5, 3, 'c' },
};

static float
half(float x)
{
	/* (round to F16's 11 bits of mantissa; no subnormals needed here) */
	int e;
	float m = frexpf(x, &e);

	return ldexpf(roundf(m * 2048) / 2048, e);
}

/* what the format keeps of v, channel c (3: alpha) */
static float
kept(enum keep k, int c, float v)
{
	switch (k) {
	case F32:      return v;
	case F16:      return half(v);
	case SNORM8:   return roundf(fminf(fmaxf(v, -1), 1) * 127) / 127;
	case UNORM16:  return roundf(fminf(fmaxf(v, 0), 1) * 65535) / 65535;
	case UNORM10_2:
		return c == 3 ? roundf(fminf(fmaxf(v, 0), 1) * 3) / 3 :
		                roundf(fminf(fmaxf(v, 0), 1) * 1023) / 1023;
	case E5:       return fmaxf(v, 0);
	}
	return v;
}

/* what a lookup of the texel gives: the channels the format has, as GL's
 * base formats spread them (RGB: alpha 1; luminance L L L 1; ...) */
static void
looked_up(const struct fmt_case *f, const float t[4], float out[4])
{
	float r = kept(f->keep, 0, t[0]), g = kept(f->keep, 1, t[1]);
	float b = kept(f->keep, 2, t[2]), a = kept(f->keep, 3, t[3]);

	switch (f->kind) {
	case 'l': out[0] = out[1] = out[2] = r, out[3] = 1; break;
	case 'a': out[0] = out[1] = out[2] = 0, out[3] = kept(f->keep, 3, t[0]); break;
	case 'L': out[0] = out[1] = out[2] = r, out[3] = kept(f->keep, 3, t[1]); break;
	case 'i': out[0] = out[1] = out[2] = out[3] = r; break;
	default:
		out[0] = r, out[1] = g, out[2] = b, out[3] = f->channels == 4 ? a : 1;
	}
}

static int
to8(float x)
{
	return (int)(fminf(fmaxf(x, 0), 1) * 255 + 0.5f);
}

static GLuint
compile(const char *vs, const char *fs)
{
	const char *src[2] = { vs, fs };
	GLuint p = glCreateProgram();
	char log[512];
	GLint ok;

	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);

		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			glGetShaderInfoLog(s, sizeof(log), NULL, log);
			fprintf(stderr, "glfmt: %s\n", log);
		}
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "a_position");
	glLinkProgram(p);
	return p;
}

static GLuint
program(void)
{
	static const char *vs =
		"#version 120\n"
		"attribute vec4 a_position;\n"
		"varying vec2 v;\n"
		"void main() { gl_Position = a_position; v = a_position.xy * 0.5 + 0.5; }\n";
	static const char *fs =
		"#version 120\n"
		"varying vec2 v;\nuniform sampler2D t;\nuniform vec4 scale, bias;\n"
		"void main() { gl_FragColor = texture2D(t, v) * scale + bias; }\n";
	const char *src[2] = { vs, fs };
	GLuint p = glCreateProgram();
	char log[512];
	GLint ok;

	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);

		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			glGetShaderInfoLog(s, sizeof(log), NULL, log);
			fprintf(stderr, "glfmt: %s\n", log);
		}
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "a_position");
	glLinkProgram(p);
	glUseProgram(p);
	glUniform1i(glGetUniformLocation(p, "t"), 0);
	return p;
}

static void
quad(void)
{
	static const float v[] = { -1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1 };

	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
	glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* One case: a 2 x 2 texture of the four texels t (RGBA each, as floats),
 * scaled and biased into [0, 1]; filtered: GL_LINEAR, and each pixel then
 * what the four texels around its sample point give */
static void
run(const struct fmt_case *f, const char *what, const float t[4][4], float scale, float bias,
    int filtered, int tolerance)
{
	char name[64];
	float data[4][4], tex[4][4];
	int bad = 0, at = -1;
	float want[4];
	GLuint id;

	snprintf(name, sizeof(name), "%s %s", f->name, what);
	if (only && strncmp(name, only, strlen(only)))
		return;
	/* the data as the format's upload takes it: its channels in order */
	for (int i = 0; i < 4; i++)
		for (int c = 0; c < 4; c++)
			data[i][c] = t[i][c];
	if (f->fmt == GL_RGB)
		for (int i = 0; i < 4; i++)
			memcpy(&((float *)data)[3 * i], t[i], 3 * sizeof(float));
	/* (luminance, alpha, intensity: a value a texel; luminance alpha two) */
	if (f->kind != 'c')
		for (int i = 0; i < 4; i++)
			for (int c = 0; c < (f->kind == 'L' ? 2 : 1); c++)
				((float *)data)[(f->kind == 'L' ? 2 : 1) * i + c] = t[i][c];
	while (glGetError())
		;
	glGenTextures(1, &id);
	glBindTexture(GL_TEXTURE_2D, id);
	glTexImage2D(GL_TEXTURE_2D, 0, f->ifmt, 2, 2, 0, f->kind == 'c' ? f->fmt :
	             f->kind == 'a' ? GL_ALPHA : f->kind == 'L' ? GL_LUMINANCE_ALPHA : GL_LUMINANCE,
	             GL_FLOAT, data);
	if (glGetError()) {
		printf("%-30s not here\n", name);
		skipped++;
		glDeleteTextures(1, &id);
		return;
	}
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filtered ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filtered ? GL_LINEAR : GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glUniform4f(glGetUniformLocation(prog, "scale"), scale, scale, scale, scale);
	glUniform4f(glGetUniformLocation(prog, "bias"), bias, bias, bias, bias);
	quad();
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	for (int i = 0; i < 4; i++)
		looked_up(f, t[i], tex[i]);
	for (int i = 0; i < W * H; i++) {
		float s = (i % W + 0.5f) / W, u = (i / W + 0.5f) / H;
		int off = 0;

		if (filtered) {
			/* (texel centres at 0.25 and 0.75; clamped outside them) */
			float fx = fminf(fmaxf(s * 2 - 0.5f, 0), 1), fy = fminf(fmaxf(u * 2 - 0.5f, 0), 1);

			for (int c = 0; c < 4; c++) {
				float lo = tex[0][c] + (tex[1][c] - tex[0][c]) * fx;
				float hi = tex[2][c] + (tex[3][c] - tex[2][c]) * fx;

				want[c] = (lo + (hi - lo) * fy) * scale + bias;
			}
		} else {
			/* (pixels near a texel's edge left out) */
			if (fabsf(s - 0.5f) < 1.5f / W || fabsf(u - 0.5f) < 1.5f / H)
				continue;
			for (int c = 0; c < 4; c++)
				want[c] = tex[(u > 0.5f) * 2 + (s > 0.5f)][c] * scale + bias;
		}
		for (int c = 0; c < 4; c++)
			off |= abs(px[4 * i + c] - to8(want[c])) > tolerance;
		if (off && at < 0)
			at = i;
		bad += off;
	}
	glDeleteTextures(1, &id);
	cases++;
	if (!bad) {
		printf("%-30s ok\n", name);
		return;
	}
	failed++;
	printf("%-30s WRONG: %d pixels, the first at %d, %d: %d %d %d %d\n", name, bad, at % W, at / W,
	       px[4 * at], px[4 * at + 1], px[4 * at + 2], px[4 * at + 3]);
}

/* the colour a target of this many channels keeps of rgba, as ReadPixels
 * hands it back (missing channels 0, alpha 1) */
static void
kept_target(int channels, const float rgba[4], float out[4])
{
	for (int c = 0; c < 3; c++)
		out[c] = c < channels ? roundf(rgba[c] * 255) / 255 : 0;
	out[3] = channels == 4 ? roundf(rgba[3] * 255) / 255 : 1;
}

/* GLFMT_DEBUG: a target's left and right pixel after each step */
static void
step(const char *what)
{
	uint8_t l[4], r[4];

	if (getenv("GLFMT_DUMP")) {
		static uint8_t all[W * H * 4];

		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, all);
		printf("  %s, by row (. as cleared, # as drawn, x mixed):\n   ", what);
		for (int y = 0; y < H; y++) {
			int n = 0;

			for (int x = 0; x < W; x++)
				n += all[4 * (y * W + x)] == 0x99;
			putchar(n == 0 ? '.' : n == W ? '#' : 'x');
		}
		printf("\n   row 0's first 32 pixels' R: ");
		for (int x = 0; x < 32; x++)
			printf("%c", all[4 * x] == 0x99 ? '#' : '.');
		printf("\n");
		printf("  %s, R G of rows 0, 1, 31, 32, 63 (every 4th pixel):\n", what);
		for (int y = 0; y < H; y++) {
			if (y != 0 && y != 1 && y != 31 && y != 32 && y != 63)
				continue;
			printf("   %2d:", y);
			for (int x = 0; x < W; x += 4)
				printf(" %02x%02x", all[4 * (y * W + x)], all[4 * (y * W + x) + 1]);
			printf("\n");
		}
	}
	if (!getenv("GLFMT_DEBUG"))
		return;
	glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, l);
	glReadPixels(W - 1, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, r);
	printf("  %-12s left %3d %3d %3d %3d  right %3d %3d %3d %3d\n", what, l[0], l[1], l[2], l[3],
	       r[0], r[1], r[2], r[3]);
}

/* A render target of the format: cleared, a render later its right half
 * drawn, then a quad added over all of it (blending ONE, ONE) */
static void
target(const char *name, GLenum ifmt, GLenum fmt, int channels, GLuint fbo_back)
{
	static const char *vs =
		"#version 120\nattribute vec4 a_position;\n"
		"void main() { gl_Position = a_position; }\n";
	static const char *fs =
		"#version 120\nuniform vec4 colour;\n"
		"void main() { gl_FragColor = colour; }\n";
	static const float half_quad[] = { 0, -1, 1, -1, 1, 1, 0, -1, 1, 1, 0, 1 };
	const float clear[4] = { 0.25f, 0.5f, 0.75f, 1 }, drawn[4] = { 0.6f, 0.2f, 0.1f, 0.5f };
	const float added[4] = { 0.125f, 0.25f, 0.5f, 0 }, scaled[4] = { 0.5f, 0.75f, 0.25f, 1 };
	char full[64];
	GLuint tex, fbo, p;
	int bad = 0, at = -1;

	snprintf(full, sizeof(full), "target %s", name);
	if (only && strncmp(full, only, strlen(only)))
		return;
	while (glGetError())
		;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, ifmt, W, H, 0, fmt, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	if (glGetError() || glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		printf("%-30s not here\n", full);
		skipped++;
		glBindFramebuffer(GL_FRAMEBUFFER, fbo_back);
		return;
	}
	p = compile(vs, fs);
	glUseProgram(p);
	glClearColor(clear[0], clear[1], clear[2], clear[3]);
	glClear(GL_COLOR_BUFFER_BIT);
	if (!getenv("GLFMT_ONE_RENDER")) {
		glFinish();
		step("cleared");
	}
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, half_quad);
	glUniform4fv(glGetUniformLocation(p, "colour"), 1, drawn);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glFinish();
	step("half drawn");
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);
	glUniform4fv(glGetUniformLocation(p, "colour"), 1, added);
	quad();
	glFinish();
	step("added");
	/* and a render later times the target's alpha (1 where it has none:
	 * the background load's) */
	glBlendFunc(GL_DST_ALPHA, GL_ZERO);
	glUniform4fv(glGetUniformLocation(p, "colour"), 1, scaled);
	quad();
	glDisable(GL_BLEND);
	step("dst alpha");
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	for (int i = 0; i < W * H; i++) {
		float k[4], want[4];
		int off = 0;

		kept_target(channels, (i % W) < W / 2 ? clear : drawn, k);
		for (int c = 0; c < 4; c++)
			want[c] = c < channels ? k[c] + added[c] : k[c];
		/* (the target's alpha: what it keeps, 1 if none) */
		for (int c = 0; c < 4; c++)
			want[c] = c < channels || (c == 3 && channels == 4) ?
			          roundf(scaled[c] * 255) / 255 * (channels == 4 ? fminf(want[3], 1) : 1) :
			          want[c];
		for (int c = 0; c < 4; c++)
			off |= abs(px[4 * i + c] - to8(want[c])) > 2;
		if (off && at < 0)
			at = i;
		bad += off;
	}
	cases++;
	if (bad) {
		failed++;
		printf("%-30s WRONG: %d pixels, the first at %d, %d: %d %d %d %d\n", full, bad, at % W,
		       at / W, px[4 * at], px[4 * at + 1], px[4 * at + 2], px[4 * at + 3]);
	} else {
		printf("%-30s ok\n", full);
	}
	/* and sampled: what was drawn, through the other program, to RGBA8 */
	glBindFramebuffer(GL_FRAMEBUFFER, fbo_back);
	glUseProgram(prog);
	glUniform4f(glGetUniformLocation(prog, "scale"), 1, 1, 1, 1);
	glUniform4f(glGetUniformLocation(prog, "bias"), 0, 0, 0, 0);
	glBindTexture(GL_TEXTURE_2D, tex);
	quad();
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	bad = 0, at = -1;
	for (int i = 0; i < W * H; i++) {
		float k[4], want[4];
		int off = 0;

		kept_target(channels, (i % W) < W / 2 ? clear : drawn, k);
		for (int c = 0; c < 4; c++)
			want[c] = c < channels ? k[c] + added[c] : k[c];
		for (int c = 0; c < 4; c++)
			want[c] = c < channels || (c == 3 && channels == 4) ?
			          roundf(scaled[c] * 255) / 255 * (channels == 4 ? fminf(want[3], 1) : 1) :
			          want[c];
		for (int c = 0; c < 4; c++)
			off |= abs(px[4 * i + c] - to8(want[c])) > 2;
		if (off && at < 0)
			at = i;
		bad += off;
	}
	cases++;
	if (bad) {
		failed++;
		printf("%-30s WRONG: %d pixels, the first at %d, %d: %d %d %d %d\n", "  sampled", bad,
		       at % W, at / W, px[4 * at], px[4 * at + 1], px[4 * at + 2], px[4 * at + 3]);
	} else {
		printf("%-30s ok\n", "  sampled");
	}
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &tex);
	glDeleteProgram(p);
}

static float
to_srgb(float c)
{
	c = fminf(fmaxf(c, 0), 1);
	return c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1 / 2.4f) - 0.055f;
}

static float
from_srgb(float c)
{
	return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

/* An sRGB target (GL_SRGB8_ALPHA8, FRAMEBUFFER_SRGB on): cleared, half
 * drawn a render later, added to (blending in linear: the target decoded,
 * the sum encoded), read back as its bytes */
static void
target_srgb(const char *name)
{
	static const char *vs =
		"#version 120\nattribute vec4 a_position;\n"
		"void main() { gl_Position = a_position; }\n";
	static const char *fs =
		"#version 120\nuniform vec4 colour;\n"
		"void main() { gl_FragColor = colour; }\n";
	static const float half_quad[] = { 0, -1, 1, -1, 1, 1, 0, -1, 1, 1, 0, 1 };
	const float clear[4] = { 0.05f, 0.25f, 0.5f, 1 }, drawn[4] = { 0.6f, 0.02f, 0.3f, 0.5f };
	const float added[4] = { 0.125f, 0.25f, 0.2f, 0 };
	char full[64];
	GLuint tex, fbo, p, back;
	int bad = 0, at = -1;

	snprintf(full, sizeof(full), "target %s", name);
	if (only && strncmp(full, only, strlen(only)))
		return;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, (GLint *)&back);
	while (glGetError())
		;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	if (glGetError() || glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		printf("%-30s not here\n", full);
		skipped++;
		glBindFramebuffer(GL_FRAMEBUFFER, back);
		return;
	}
	glEnable(GL_FRAMEBUFFER_SRGB);
	p = compile(vs, fs);
	glUseProgram(p);
	glClearColor(clear[0], clear[1], clear[2], clear[3]);
	glClear(GL_COLOR_BUFFER_BIT);
	glFinish();
	step("cleared");
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, half_quad);
	glUniform4fv(glGetUniformLocation(p, "colour"), 1, drawn);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glFinish();
	step("half drawn");
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);
	glUniform4fv(glGetUniformLocation(p, "colour"), 1, added);
	quad();
	glDisable(GL_BLEND);
	step("added");
	glDisable(GL_FRAMEBUFFER_SRGB);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	for (int i = 0; i < W * H; i++) {
		const float *k = (i % W) < W / 2 ? clear : drawn;
		int off = 0;

		for (int c = 0; c < 4; c++) {
			/* (stored encoded and decoded back as 8 bits; alpha linear) */
			float d = c < 3 ? from_srgb(to8(to_srgb(k[c])) / 255.0f) : to8(k[c]) / 255.0f;
			float want = c < 3 ? to_srgb(d + added[c]) : fminf(d + added[c], 1);

			off |= abs(px[4 * i + c] - to8(want)) > 3;
		}
		if (off && at < 0)
			at = i;
		bad += off;
	}
	cases++;
	if (bad) {
		failed++;
		printf("%-30s WRONG: %d pixels, the first at %d, %d: %d %d %d %d\n", full, bad, at % W,
		       at / W, px[4 * at], px[4 * at + 1], px[4 * at + 2], px[4 * at + 3]);
	} else {
		printf("%-30s ok\n", full);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, back);
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &tex);
	glDeleteProgram(p);
}

/* GLFMT_DEBUG: a float target's left and right pixel after each step */
static void
stepf(const char *what)
{
	float l[4], r[4];

	if (!getenv("GLFMT_DEBUG"))
		return;
	glReadPixels(0, 0, 1, 1, GL_RGBA, GL_FLOAT, l);
	glReadPixels(W - 1, 0, 1, 1, GL_RGBA, GL_FLOAT, r);
	printf("  %-12s left %g %g %g %g  right %g %g %g %g\n", what, l[0], l[1], l[2], l[3],
	       r[0], r[1], r[2], r[3]);
}

/* what a float target of this many channels keeps of v, channel c --
 * packed: R11G11B10F's, no sign and 6 or 5 bits of mantissa -- as read
 * back (missing channels 0, alpha 1) */
static float
kept_float(int channels, int packed, int c, float v)
{
	if (c >= channels)
		return c == 3 ? 1 : 0;
	return packed ? fmaxf(v, 0) : v;
}

/* A render target of floats: what target() does, with values past [0, 1]
 * -- not clamped -- read back as floats */
static void
target_float(const char *name, GLenum ifmt, int channels, int packed)
{
	static const char *vs =
		"#version 120\nattribute vec4 a_position;\n"
		"void main() { gl_Position = a_position; }\n";
	static const char *fs =
		"#version 120\nuniform vec4 colour;\n"
		"void main() { gl_FragColor = colour; }\n";
	static const float half_quad[] = { 0, -1, 1, -1, 1, 1, 0, -1, 1, 1, 0, 1 };
	static float all[W * H * 4];
	const float clear[4] = { 0.25f, 0.5f, 0.75f, 1 }, drawn[4] = { 2.5f, -1.5f, 0.125f, 0.5f };
	const float added[4] = { 0.125f, 0.25f, 4, 0 }, scaled[4] = { 0.5f, 3, -2, 1 };
	char full[64];
	GLuint tex, fbo, p, back;
	int bad = 0, at = -1;

	snprintf(full, sizeof(full), "target %s", name);
	if (only && strncmp(full, only, strlen(only)))
		return;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, (GLint *)&back);
	while (glGetError())
		;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, ifmt, W, H, 0, GL_RGBA, GL_FLOAT, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	if (glGetError() || glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		printf("%-30s not here\n", full);
		skipped++;
		glBindFramebuffer(GL_FRAMEBUFFER, back);
		return;
	}
	/* (GL clamps fragment colours of fixed point targets only) */
	glClampColorARB(GL_CLAMP_FRAGMENT_COLOR_ARB, GL_FALSE);
	glClampColorARB(GL_CLAMP_READ_COLOR_ARB, GL_FALSE);
	p = compile(vs, fs);
	glUseProgram(p);
	glClearColor(clear[0], clear[1], clear[2], clear[3]);
	glClear(GL_COLOR_BUFFER_BIT);
	glFinish();
	stepf("cleared");
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, half_quad);
	glUniform4fv(glGetUniformLocation(p, "colour"), 1, drawn);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glFinish();
	stepf("half drawn");
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);
	glUniform4fv(glGetUniformLocation(p, "colour"), 1, added);
	quad();
	glFinish();
	stepf("added");
	glBlendFunc(GL_DST_ALPHA, GL_ZERO);
	glUniform4fv(glGetUniformLocation(p, "colour"), 1, scaled);
	quad();
	glDisable(GL_BLEND);
	stepf("dst alpha");
	glReadPixels(0, 0, W, H, GL_RGBA, GL_FLOAT, all);
	for (int i = 0; i < W * H; i++) {
		const float *k = (i % W) < W / 2 ? clear : drawn;
		float a = channels == 4 ? k[3] + added[3] : 1;
		int off = 0;

		for (int c = 0; c < 4; c++) {
			float want = kept_float(channels, packed, c, scaled[c] * a);

			off |= fabsf(all[4 * i + c] - want) > 0.01f + (packed ? 0.04f : 0.002f) * fabsf(want);
		}
		if (off && at < 0)
			at = i;
		bad += off;
	}
	cases++;
	if (bad) {
		failed++;
		printf("%-30s WRONG: %d pixels, the first at %d, %d: %g %g %g %g\n", full, bad,
		       at % W, at / W, all[4 * at], all[4 * at + 1], all[4 * at + 2], all[4 * at + 3]);
	} else {
		printf("%-30s ok\n", full);
	}
	glClampColorARB(GL_CLAMP_FRAGMENT_COLOR_ARB, GL_FIXED_ONLY_ARB);
	glClampColorARB(GL_CLAMP_READ_COLOR_ARB, GL_FIXED_ONLY_ARB);
	glBindFramebuffer(GL_FRAMEBUFFER, back);
	glDeleteFramebuffers(1, &fbo);
	glDeleteTextures(1, &tex);
	glDeleteProgram(p);
}

int
main(int argc, char **argv)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_MAJOR_VERSION, 2, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE };
	/* values past [0, 1] (scale 1/50, bias 0.5), within it (exactly, as
	 * far as 8 bits show), and for the formats that keep no sign */
	static const float range[4][4] = {
		{ -20, 12.5f, 3.25f, 1 }, { 18, -7.75f, 0.5f, -2 },
		{ 0.25f, 24, -16, 9 }, { -1.5f, 2, 20, -24 },
	};
	static const float unit[4][4] = {
		{ 0.1f, 0.9f, 0.5f, 1 }, { 0.75f, 0.25f, 0.6f, 0.2f },
		{ 0.33f, 0.66f, 0.05f, 0.95f }, { 1, 0, 0.45f, 0.55f },
	};
	static const float signed_unit[4][4] = {
		{ -0.8f, 0.9f, -0.5f, 1 }, { 0.75f, -0.25f, 0.6f, -1 },
		{ -0.33f, 0.66f, 0.05f, -0.95f }, { 1, 0, -0.45f, 0.55f },
	};
	static const float positive[4][4] = {
		{ 20, 12.5f, 3.25f, 1 }, { 18, 7.75f, 0.5f, 2 },
		{ 0.25f, 24, 16, 9 }, { 1.5f, 2, 20, 24 },
	};
	GLuint rt, fbo;
	EGLDisplay dpy;
	EGLContext ctx;

	only = argc > 1 ? argv[1] : NULL;
	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_API))
		return fprintf(stderr, "glfmt: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glfmt: no OpenGL 2.1 context\n"), 2;
	printf("%s, %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		return fprintf(stderr, "glfmt: the target is not complete\n"), 2;
	glViewport(0, 0, W, H);
	prog = program();
	glActiveTexture(GL_TEXTURE0);

	for (unsigned i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
		const struct fmt_case *f = &fmts[i];

		if (f->keep == F32 || f->keep == F16) {
			run(f, "range", range, 1 / 50.0f, 0.5f, 0, 2);
			run(f, "unit", unit, 1, 0, 0, 2);
			run(f, "filtered", unit, 1, 0, 1, 3);
		} else if (f->keep == SNORM8) {
			run(f, "unit", signed_unit, 0.5f, 0.5f, 0, 2);
			run(f, "filtered", signed_unit, 0.5f, 0.5f, 1, 3);
		} else if (f->keep == E5) {
			run(f, "range", positive, 1 / 25.0f, 0, 0, 2);
			run(f, "unit", unit, 1, 0, 0, 2);
		} else {
			run(f, "unit", unit, 1, 0, 0, 2);
			run(f, "filtered", unit, 1, 0, 1, 3);
		}
	}
	target("rgba8", GL_RGBA8, GL_RGBA, 4, fbo);
	target("r8", GL_R8, GL_RED, 1, fbo);
	target("rg8", GL_RG8, GL_RG, 2, fbo);
	target_srgb("srgb8-alpha8");
	target_float("rgba16f", GL_RGBA16F_ARB, 4, 0);
	target_float("rg16f", GL_RG16F, 2, 0);
	target_float("r16f", GL_R16F, 1, 0);
	target_float("r32f", GL_R32F, 1, 0);
	target_float("rg32f", GL_RG32F, 2, 0);
	target_float("r11g11b10f", GL_R11F_G11F_B10F_EXT, 3, 1);
	printf("%d of %d cases right (%d formats not here)\n", cases - failed, cases, skipped);
	return failed ? 1 : 0;
}
