/* gltex -- texture lookups in compiled fragment shaders (docs/research/
 * p105-mesa.md, M14): small textures whose texels are known, sampled over a
 * full-screen quad with v = (s, t, 1 - s, 1 - t), checked on a grid of
 * pixels against the texel each should land on.  Pixels close to a texel's
 * edge are left out (which side they fall is a rounding question, not a
 * texturing one).
 *
 *   sgx-gl gltex [CASE...]       (a name, or a prefix; all by default)
 *
 * Exit status 0 when every case passed.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 768
#define H 1024
#define GRID 37      /* not a divisor of the texture sizes */

/* a texture: texel (x, y) of a w x h image, RGBA bytes */
typedef void (*texel_fn)(int x, int y, uint8_t c[4]);

static void tx_quad(int x, int y, uint8_t c[4])      /* 4x4: a colour each */
{
	c[0] = x * 80, c[1] = y * 80, c[2] = 200 - x * 40, c[3] = 255 - y * 20;
}

static void tx_strip(int x, int y, uint8_t c[4])     /* 64x16 */
{
	c[0] = x * 4, c[1] = y * 16, c[2] = 255 - x * 4, c[3] = 255;
}

static void tx_other(int x, int y, uint8_t c[4])     /* 2x2 */
{
	c[0] = x ? 255 : 0, c[1] = y ? 255 : 0, c[2] = 64, c[3] = 255;
}

static void tx_lum(int x, int y, uint8_t c[4])       /* 4x4 L8 */
{
	c[0] = c[1] = c[2] = (x + 4 * y) * 16, c[3] = 255;
}

static void tx_565(int x, int y, uint8_t c[4])       /* 2x2 RGB565: exact values */
{
	c[0] = x ? 255 : 0, c[1] = y ? 255 : 0, c[2] = (x ^ y) ? 255 : 0, c[3] = 255;
}

struct tex {
	int w, h;
	GLenum format, type;    /* GL_RGBA/UNSIGNED_BYTE, GL_LUMINANCE, GL_RGB/565 */
	texel_fn fn;
};

static const struct tex T_QUAD = { 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, tx_quad };
static const struct tex T_STRIP = { 64, 16, GL_RGBA, GL_UNSIGNED_BYTE, tx_strip };
static const struct tex T_OTHER = { 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, tx_other };
static const struct tex T_LUM = { 4, 4, GL_LUMINANCE, GL_UNSIGNED_BYTE, tx_lum };
static const struct tex T_565 = { 2, 2, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, tx_565 };

/* the expected colour at s, t; false where the pixel is too near an edge */
typedef int (*ref_fn)(float s, float t, float c[4]);

static const struct tex *cur[2];

/* nearest texel of texture k at (u, v); false near an edge */
static int texel(int k, float u, float v, int repeat, float c[4])
{
	const struct tex *T = cur[k];
	float fu = u * T->w, fv = v * T->h;
	int x, y;
	uint8_t b[4];

	if (fabsf(fu - floorf(fu) - 0.5f) > 0.4f || fabsf(fv - floorf(fv) - 0.5f) > 0.4f)
		return 0;
	x = (int)floorf(fu), y = (int)floorf(fv);
	if (repeat) {
		x = ((x % T->w) + T->w) % T->w;
		y = ((y % T->h) + T->h) % T->h;
	} else {
		x = x < 0 ? 0 : x >= T->w ? T->w - 1 : x;
		y = y < 0 ? 0 : y >= T->h ? T->h - 1 : y;
	}
	T->fn(x, y, b);
	for (int i = 0; i < 4; i++)
		c[i] = b[i] / 255.0f;
	return 1;
}

static int r_plain(float s, float t, float c[4]) { return texel(0, s, t, 0, c); }
static int r_repeat(float s, float t, float c[4]) { return texel(0, s * 2.5f, t * 1.5f, 1, c); }
static int r_swizzle(float s, float t, float c[4])
{
	float a[4];

	if (!texel(0, t, s, 0, a))
		return 0;
	c[0] = a[2], c[1] = a[1], c[2] = a[0], c[3] = a[3];
	return 1;
}
static int r_math(float s, float t, float c[4])
{
	static const float u[4] = { 0.5f, 1.0f, 0.25f, 1.0f };

	if (!texel(0, s, t, 0, c))
		return 0;
	for (int i = 0; i < 4; i++)
		c[i] = c[i] * u[i] + 0.1f * s;
	return 1;
}
static int r_two(float s, float t, float c[4])
{
	float a[4], b[4];

	if (!texel(0, s, t, 0, a) || !texel(1, s, t, 0, b))
		return 0;
	for (int i = 0; i < 4; i++)
		c[i] = a[i] * 0.5f + b[i] * 0.5f;
	return 1;
}
static int r_proj(float s, float t, float c[4]) { return texel(0, s, t, 0, c); }

static const struct test {
	const char *name, *body;
	const struct tex *t0, *t1;
	int repeat;
	ref_fn ref;
} tests[] = {
	{ "plain", "c = texture2D(t0, v.xy);", &T_QUAD, NULL, 0, r_plain },
	{ "rect", "c = texture2D(t0, v.xy);", &T_STRIP, NULL, 0, r_plain },
	{ "repeat", "c = texture2D(t0, v.xy * vec2(2.5, 1.5));", &T_QUAD, NULL, 1, r_repeat },
	{ "swizzle", "c = texture2D(t0, v.yx).bgra;", &T_QUAD, NULL, 0, r_swizzle },
	{ "math", "c = texture2D(t0, v.xy) * u + 0.1 * v.x;", &T_QUAD, NULL, 0, r_math },
	{ "two", "c = texture2D(t0, v.xy) * 0.5 + texture2D(t1, v.xy) * 0.5;", &T_QUAD, &T_OTHER, 0, r_two },
	{ "luminance", "c = texture2D(t0, v.xy);", &T_LUM, NULL, 0, r_plain },
	{ "rgb565", "c = texture2D(t0, v.xy);", &T_565, NULL, 0, r_plain },
	{ "proj", "c = texture2DProj(t0, vec3(v.xy * 2.0, 2.0));", &T_QUAD, NULL, 0, r_proj },
};

static const char *vs_src =
	"attribute vec4 p;\n"
	"varying vec4 v;\n"
	"void main() {\n"
	"  vec2 st = p.xy * 0.5 + 0.5;\n"
	"  v = vec4(st, 1.0 - st);\n"
	"  gl_Position = p;\n"
	"}\n";

static GLuint build(const char *body)
{
	static char fs[2048];
	const char *src[2] = { vs_src, fs };
	GLenum type[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	GLuint p = glCreateProgram();
	char log[1024];
	GLint ok;

	snprintf(fs, sizeof(fs),
		 "precision highp float;\n"
		 "varying vec4 v;\n"
		 "uniform sampler2D t0, t1;\n"
		 "uniform vec4 u;\n"
		 "void main() { vec4 c; %s gl_FragColor = c; }\n", body);
	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(type[i]);

		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			glGetShaderInfoLog(s, sizeof(log), NULL, log);
			fprintf(stderr, "gltex: shader: %s\n", log);
			return 0;
		}
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "p");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		glGetProgramInfoLog(p, sizeof(log), NULL, log);
		fprintf(stderr, "gltex: link: %s\n", log);
		return 0;
	}
	return p;
}

/* a texture object with T's texels, on unit `unit` */
static GLuint make(const struct tex *T, int unit, int repeat)
{
	static uint8_t data[64 * 64 * 4];
	GLuint t;
	int n = 0;

	for (int y = 0; y < T->h; y++) {
		for (int x = 0; x < T->w; x++) {
			uint8_t c[4];

			T->fn(x, y, c);
			if (T->format == GL_LUMINANCE) {
				data[n++] = c[0];
			} else if (T->type == GL_UNSIGNED_SHORT_5_6_5) {
				uint16_t v = (c[0] >> 3) << 11 | (c[1] >> 2) << 5 | c[2] >> 3;

				memcpy(data + n, &v, 2);
				n += 2;
			} else {
				memcpy(data + n, c, 4);
				n += 4;
			}
		}
	}
	glActiveTexture(GL_TEXTURE0 + unit);
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, T->format, T->w, T->h, 0, T->format, T->type, data);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
	return t;
}

static int to8(float x)
{
	x = x < 0 ? 0 : x > 1 ? 1 : x;
	return (int)(x * 255 + 0.5f);
}

static int wanted(int argc, char **argv, const char *name)
{
	if (argc < 2)
		return 1;
	for (int i = 1; i < argc; i++)
		if (!strncmp(name, argv[i], strlen(argv[i])))
			return 1;
	return 0;
}

int main(int argc, char **argv)
{
	static const float quad[] = { -1, -1, 0, 1,  1, -1, 0, 1,  1, 1, 0, 1,
	                              -1, -1, 0, 1,  1, 1, 0, 1,  -1, 1, 0, 1 };
	static const float U[4] = { 0.5f, 1.0f, 0.25f, 1.0f };
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	uint8_t *px = malloc(W * H * 4);
	int run = 0, failed = 0;
	EGLDisplay dpy;
	EGLContext ctx;
	GLuint rt, fbo;

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "gltex: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "gltex: no GLES 2 context\n"), 2;

	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glViewport(0, 0, W, H);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, quad);

	for (unsigned k = 0; k < sizeof(tests) / sizeof(tests[0]); k++) {
		const struct test *T = &tests[k];
		int worst = 0, wx = 0, wy = 0, got[4] = { 0 }, want[4] = { 0 }, checked = 0;
		GLuint p, t0, t1 = 0;

		if (!wanted(argc, argv, T->name))
			continue;
		run++;
		if (!(p = build(T->body))) {
			printf("%-12s does not build\n", T->name);
			failed++;
			continue;
		}
		cur[0] = T->t0, cur[1] = T->t1;
		t0 = make(T->t0, 0, T->repeat);
		if (T->t1)
			t1 = make(T->t1, 1, T->repeat);
		glUseProgram(p);
		glUniform1i(glGetUniformLocation(p, "t0"), 0);
		glUniform1i(glGetUniformLocation(p, "t1"), 1);
		glUniform4fv(glGetUniformLocation(p, "u"), 1, U);
		glClearColor(0, 0, 0, 0);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);

		for (int gy = 0; gy < GRID; gy++) {
			for (int gx = 0; gx < GRID; gx++) {
				int x = (gx * W + W / 2) / GRID, y = (gy * H + H / 2) / GRID;
				float s = (x + 0.5f) / W, t = (y + 0.5f) / H, c[4];
				const uint8_t *q = px + (y * W + x) * 4;

				if (!T->ref(s, t, c))
					continue;
				checked++;
				for (int i = 0; i < 4; i++) {
					int d = abs(q[i] - to8(c[i]));

					if (d > worst) {
						worst = d, wx = x, wy = y;
						for (int j = 0; j < 4; j++)
							got[j] = q[j], want[j] = to8(c[j]);
					}
				}
			}
		}
		if (worst <= 2 && checked) {
			printf("%-12s ok (%d pixels, largest difference %d)\n", T->name, checked, worst);
		} else {
			failed++;
			printf("%-12s WRONG by %d at %d,%d: %02x %02x %02x %02x, want %02x %02x %02x %02x\n",
			       T->name, worst, wx, wy, got[0], got[1], got[2], got[3],
			       want[0], want[1], want[2], want[3]);
		}
		glDeleteTextures(1, &t0);
		if (t1)
			glDeleteTextures(1, &t1);
		glDeleteProgram(p);
	}
	printf("%d of %d cases right\n", run - failed, run);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	free(px);
	return failed ? 1 : 0;
}
