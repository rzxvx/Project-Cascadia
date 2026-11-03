/* glfetch -- texture lookups the PDS makes before the pixel program runs
 * (docs/research/p105-mesa.md, M35): texture2D at a varying's x and y, the
 * texel handed to the program.  One texture, times a colour (SDL's), the
 * coordinates read besides, two and four textures, a fifth (sampled in the
 * program), one texture twice, beside a dependent lookup, at .zw (in the
 * program), with gl_FragCoord, in an if/else on a varying, in a loop, a
 * mipmap's level.  4 x 4 textures over a 64 x 64 target, every pixel read
 * back against what C works out.
 *
 *   sgx-gl glfetch [CASE]        (CASE: a name's start; all by default)
 *
 * SGX_NOFETCH=1 makes the driver sample them all in the program: the same
 * pictures.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64
#define UNITS 5

static uint8_t px[W * H * 4];
static int cases, failed;
static const char *only;

static const char *vs =
	"attribute vec4 a_position;\n"
	"varying vec2 v, w;\n"
	"varying vec4 col, q;\n"
	"void main() {\n"
	"  vec2 t = a_position.xy * 0.5 + 0.5;\n"
	"  gl_Position = a_position;\n"
	"  v = t;\n"
	"  w = vec2(1.0 - t.x, t.y);\n"
	"  col = vec4(t.x, t.y, 0.5, 1.0);\n"
	"  q = vec4(t.y, t.x, 1.0 - t.y, t.x);\n"
	"}\n";

/* texture u's texel i (0..15): bytes that tell the unit and the texel */
static void
texel_bytes(int u, int i, uint8_t c[4])
{
	c[0] = i * 16 + u * 3;
	c[1] = 0x20 + i * 8 + u * 5;
	c[2] = 0xf0 - i * 12 - u * 7;
	c[3] = 0x80 + u * 20 + i;
}

static void
texel(int u, float s, float t, float c[4])
{
	uint8_t b[4];
	int x = (int)floorf(s * 4), y = (int)floorf(t * 4);

	texel_bytes(u, (x & 3) + 4 * (y & 3), b);
	for (int k = 0; k < 4; k++)
		c[k] = b[k] / 255.0f;
}

static GLuint
program(const char *fs_body)
{
	char fs[4096], log[512];
	const char *src[2] = { vs, fs };
	GLuint p = glCreateProgram();
	GLint ok;

	snprintf(fs, sizeof(fs), "precision mediump float;\n"
	         "varying vec2 v, w;\nvarying vec4 col, q;\n"
	         "uniform sampler2D t0, t1, t2, t3, t4, m;\nuniform int n;\n"
	         "void main() { %s }\n", fs_body);
	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);

		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			glGetShaderInfoLog(s, sizeof(log), NULL, log);
			fprintf(stderr, "glfetch: %s\n", log);
		}
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "a_position");
	glLinkProgram(p);
	glUseProgram(p);
	for (int u = 0; u < UNITS; u++) {
		char name[4] = { 't', '0' + u, 0 };

		glUniform1i(glGetUniformLocation(p, name), u);
	}
	glUniform1i(glGetUniformLocation(p, "m"), UNITS);
	glUniform1i(glGetUniformLocation(p, "n"), 2);
	return p;
}

static int
to8(float x)
{
	return (int)(fminf(fmaxf(x, 0), 1) * 255 + 0.5f);
}

static void
check(const char *name, const char *body, void (*want)(float s, float t, float rgba[4]))
{
	static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	int bad = 0, at = -1;
	float rgba[4];
	GLuint p;

	if (only && strncmp(name, only, strlen(only)))
		return;
	p = program(body);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
	glDeleteProgram(p);
	for (int i = 0; i < W * H; i++) {
		int off = 0;

		want((i % W + 0.5f) / W, (i / W + 0.5f) / H, rgba);
		for (int c = 0; c < 4; c++)
			off |= abs(px[4 * i + c] - to8(rgba[c])) > 2;
		if (off && at < 0)
			at = i;
		bad += off;
	}
	cases++;
	if (!bad) {
		printf("%-36s ok\n", name);
		return;
	}
	failed++;
	want((at % W + 0.5f) / W, (at / W + 0.5f) / H, rgba);
	printf("%-36s WRONG: %d pixels, the first at %d, %d: %d %d %d %d, want %d %d %d %d\n", name,
	       bad, at % W, at / W, px[4 * at], px[4 * at + 1], px[4 * at + 2], px[4 * at + 3],
	       to8(rgba[0]), to8(rgba[1]), to8(rgba[2]), to8(rgba[3]));
}

static void
want_one(float s, float t, float c[4])
{
	texel(0, s, t, c);
}

static void
want_colour(float s, float t, float c[4])
{
	texel(0, s, t, c);
	c[0] *= s, c[1] *= t, c[2] *= 0.5f;
}

static void
want_coords(float s, float t, float c[4])
{
	texel(0, s, t, c);
	c[0] += s * 0.25f, c[1] += t * 0.25f;
}

static void
want_two(float s, float t, float c[4])
{
	float b[4];

	texel(0, s, t, c);
	texel(1, 1 - s, t, b);
	for (int k = 0; k < 4; k++)
		c[k] = c[k] * 0.5f + b[k] * 0.5f;
}

static void
want_four(float s, float t, float c[4])
{
	float b[4][4];

	texel(0, s, t, b[0]);
	texel(1, 1 - s, t, b[1]);
	texel(2, t, s, b[2]);
	texel(3, s, t, b[3]);
	for (int k = 0; k < 4; k++)
		c[k] = (b[0][k] + b[1][k] + b[2][k] + b[3][k]) * 0.25f;
}

static void
want_five(float s, float t, float c[4])
{
	float b[5][4];

	texel(0, s, t, b[0]);
	texel(1, 1 - s, t, b[1]);
	texel(2, t, s, b[2]);
	texel(3, s, t, b[3]);
	texel(4, 1 - s, t, b[4]);
	for (int k = 0; k < 4; k++)
		c[k] = (b[0][k] + b[1][k] + b[2][k] + b[3][k] + b[4][k]) * 0.2f;
}

static void
want_twice(float s, float t, float c[4])
{
	float b[4];

	texel(0, s, t, c);
	texel(0, 1 - s, t, b);
	for (int k = 0; k < 4; k++)
		c[k] *= b[k];
}

static void
want_dependent(float s, float t, float c[4])
{
	float b[4];

	texel(0, s, t, c);
	texel(1, t, s, b);
	for (int k = 0; k < 4; k++)
		c[k] = c[k] * 0.5f + b[k] * 0.5f;
}

static void
want_zw(float s, float t, float c[4])
{
	texel(0, 1 - t, s, c);
}

static void
want_fragcoord(float s, float t, float c[4])
{
	texel(0, s, t, c);
	c[0] = c[0] * 0.5f + s * 0.5f;
	c[1] = c[1] * 0.5f + t * 0.5f;
	c[2] *= 0.5f, c[3] *= 0.5f;
}

static void
want_branch(float s, float t, float c[4])
{
	if (t > 0.5f)
		texel(0, s, t, c);
	else
		texel(1, 1 - s, t, c);
}

static void
want_loop(float s, float t, float c[4])
{
	texel(0, s, t, c);
}

/* the mipmapped texture's level 1 (green) */
static void
want_level1(float s, float t, float c[4])
{
	(void)s, (void)t;
	c[0] = 0, c[1] = 1, c[2] = 0, c[3] = 1;
}

int
main(int argc, char **argv)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	GLuint rt, fbo, tex[UNITS], mip;
	EGLDisplay dpy;
	EGLContext ctx;

	only = argc > 1 ? argv[1] : NULL;
	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glfetch: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glfetch: no GLES 2 context\n"), 2;
	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glViewport(0, 0, W, H);
	glEnableVertexAttribArray(0);

	glGenTextures(UNITS, tex);
	for (int u = 0; u < UNITS; u++) {
		uint8_t texels[16][4];

		for (int i = 0; i < 16; i++)
			texel_bytes(u, i, texels[i]);
		glActiveTexture(GL_TEXTURE0 + u);
		glBindTexture(GL_TEXTURE_2D, tex[u]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}
	/* a 128 x 128 texture, a colour a level: drawn at 64 x 64, level 1 */
	{
		static const uint8_t colour[8][4] = {
			{ 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 255, 255, 0, 255 },
			{ 0, 255, 255, 255 }, { 255, 0, 255, 255 }, { 255, 255, 255, 255 }, { 0, 0, 0, 255 },
		};
		uint8_t *img = malloc(128 * 128 * 4);

		glActiveTexture(GL_TEXTURE0 + UNITS);
		glGenTextures(1, &mip);
		glBindTexture(GL_TEXTURE_2D, mip);
		for (int l = 0; l < 8; l++) {
			int sz = 128 >> l;

			for (int i = 0; i < sz * sz; i++)
				memcpy(img + 4 * i, colour[l], 4);
			glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA, sz, sz, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
		}
		free(img);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}

	/* GLFETCH_FS=body: that program, its pixels at (8, 8) and (40, 40) */
	if (getenv("GLFETCH_FS")) {
		static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };

		program(getenv("GLFETCH_FS"));
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		for (int i = 0; i < 2; i++) {
			uint8_t *p = px + 4 * (W * (8 + 32 * i) + 8 + 32 * i);

			printf("(%d, %d): %d %d %d %d\n", 8 + 32 * i, 8 + 32 * i, p[0], p[1], p[2], p[3]);
		}
		return 0;
	}
	check("one texture", "gl_FragColor = texture2D(t0, v);", want_one);
	check("times a colour (SDL's)", "gl_FragColor = texture2D(t0, v) * col;", want_colour);
	check("the coordinates read besides",
	      "gl_FragColor = texture2D(t0, v) + vec4(v * 0.25, 0.0, 0.0);", want_coords);
	check("two textures", "gl_FragColor = texture2D(t0, v) * 0.5 + texture2D(t1, w) * 0.5;",
	      want_two);
	check("four textures", "gl_FragColor = (texture2D(t0, v) + texture2D(t1, w) + "
	      "texture2D(t2, q.xy) + texture2D(t3, v)) * 0.25;", want_four);
	check("five textures (one in the program)", "gl_FragColor = (texture2D(t0, v) + "
	      "texture2D(t1, w) + texture2D(t2, q.xy) + texture2D(t3, v) + texture2D(t4, w)) * 0.2;",
	      want_five);
	check("one texture twice", "gl_FragColor = texture2D(t0, v) * texture2D(t0, w);", want_twice);
	check("beside a dependent lookup",
	      "gl_FragColor = texture2D(t0, v) * 0.5 + texture2D(t1, v.yx) * 0.5;", want_dependent);
	check("at .zw (in the program)", "gl_FragColor = texture2D(t0, q.zw);", want_zw);
	check("with gl_FragCoord", "gl_FragColor = texture2D(t0, v) * 0.5 + "
	      "vec4(gl_FragCoord.xy / 64.0, 0.0, 0.0) * 0.5;", want_fragcoord);
	check("in an if/else on a varying", "vec4 c; if (v.y > 0.5) c = texture2D(t0, v); "
	      "else c = texture2D(t1, w); gl_FragColor = c;", want_branch);
	check("in a loop", "vec4 c = vec4(0.0); for (int i = 0; i < n; i++) "
	      "c += texture2D(t0, v) * 0.5; gl_FragColor = c;", want_loop);
	check("a mipmap's level", "gl_FragColor = texture2D(m, v);", want_level1);
	check("a mipmap's level, mirrored", "gl_FragColor = texture2D(m, w);", want_level1);

	printf("%d of %d cases right\n", cases - failed, cases);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return failed ? 1 : 0;
}
