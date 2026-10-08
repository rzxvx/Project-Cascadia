/* glblend -- GL blending done by the compiled fragment shader (docs/research/
 * p105-mesa.md, M14): a gradient drawn without blending, then a second
 * full-screen quad blended over it, checked on a grid of pixels against the
 * same equation in C on the gradient as the 8-bit target holds it.
 *
 *   background  d = (s, t, 1 - s, 0.25 + 0.5 t)
 *   source      c = (t, 0.5, s, s)          s, t = 0..1 across the screen
 *
 *   sgx-gl glblend [CASE...]     (a name, or a prefix; all by default)
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
#define GRID 29

static const float CONST[4] = { 0.25f, 0.5f, 0.75f, 1.0f };

static const struct test {
	const char *name;
	int enable;
	GLenum eq_rgb, eq_a, src_rgb, dst_rgb, src_a, dst_a;
	GLboolean mask[4];
	int twice;      /* the source quad twice in one draw */
} tests[] = {
	{ "none", 0, GL_FUNC_ADD, GL_FUNC_ADD, GL_ONE, GL_ZERO, GL_ONE, GL_ZERO, { 1, 1, 1, 1 } },
	{ "alpha", 1, GL_FUNC_ADD, GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
	  GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, { 1, 1, 1, 1 } },
	{ "add", 1, GL_FUNC_ADD, GL_FUNC_ADD, GL_ONE, GL_ONE, GL_ONE, GL_ONE, { 1, 1, 1, 1 } },
	{ "multiply", 1, GL_FUNC_ADD, GL_FUNC_ADD, GL_DST_COLOR, GL_ZERO, GL_DST_ALPHA, GL_ZERO,
	  { 1, 1, 1, 1 } },
	{ "premultiplied", 1, GL_FUNC_ADD, GL_FUNC_ADD, GL_ONE, GL_ONE_MINUS_SRC_ALPHA,
	  GL_ONE, GL_ONE_MINUS_SRC_ALPHA, { 1, 1, 1, 1 } },
	{ "subtract", 1, GL_FUNC_SUBTRACT, GL_FUNC_SUBTRACT, GL_ONE, GL_ONE, GL_ONE, GL_ONE,
	  { 1, 1, 1, 1 } },
	{ "reverse", 1, GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_ONE, GL_ONE,
	  GL_ONE, GL_ONE, { 1, 1, 1, 1 } },
	{ "constant", 1, GL_FUNC_ADD, GL_FUNC_ADD, GL_CONSTANT_COLOR, GL_ONE_MINUS_CONSTANT_COLOR,
	  GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA, { 1, 1, 1, 1 } },
	{ "separate", 1, GL_FUNC_ADD, GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
	  GL_ONE, GL_ZERO, { 1, 1, 1, 1 } },
	{ "dstalpha", 1, GL_FUNC_ADD, GL_FUNC_ADD, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA,
	  GL_ONE, GL_ZERO, { 1, 1, 1, 1 } },
	{ "saturate", 1, GL_FUNC_ADD, GL_FUNC_ADD, GL_SRC_ALPHA_SATURATE, GL_ONE, GL_ONE, GL_ZERO,
	  { 1, 1, 1, 1 } },
	{ "keep", 1, GL_FUNC_ADD, GL_FUNC_ADD, GL_ZERO, GL_ONE, GL_ZERO, GL_ONE, { 1, 1, 1, 1 } },
	{ "colormask", 0, GL_FUNC_ADD, GL_FUNC_ADD, GL_ONE, GL_ZERO, GL_ONE, GL_ZERO, { 1, 0, 1, 0 } },
	{ "twice", 1, GL_FUNC_ADD, GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO,
	  { 1, 1, 1, 1 }, 1 },
};

static float factor(GLenum f, const float s[4], const float d[4], int i)
{
	switch (f) {
	case GL_ZERO: return 0;
	case GL_ONE: return 1;
	case GL_SRC_COLOR: return s[i];
	case GL_ONE_MINUS_SRC_COLOR: return 1 - s[i];
	case GL_DST_COLOR: return d[i];
	case GL_ONE_MINUS_DST_COLOR: return 1 - d[i];
	case GL_SRC_ALPHA: return s[3];
	case GL_ONE_MINUS_SRC_ALPHA: return 1 - s[3];
	case GL_DST_ALPHA: return d[3];
	case GL_ONE_MINUS_DST_ALPHA: return 1 - d[3];
	case GL_CONSTANT_COLOR: return CONST[i];
	case GL_ONE_MINUS_CONSTANT_COLOR: return 1 - CONST[i];
	case GL_CONSTANT_ALPHA: return CONST[3];
	case GL_ONE_MINUS_CONSTANT_ALPHA: return 1 - CONST[3];
	case GL_SRC_ALPHA_SATURATE:
		return i == 3 ? 1 : fminf(s[3], 1 - d[3]);
	}
	return 0;
}

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static int to8(float x) { return (int)(clamp01(x) * 255 + 0.5f); }

/* one blend of s over d, GL's way */
static void blend(const struct test *T, const float s[4], float d[4])
{
	float r[4];

	for (int i = 0; i < 4; i++) {
		GLenum eq = i < 3 ? T->eq_rgb : T->eq_a;
		float fs = factor(i < 3 ? T->src_rgb : T->src_a, s, d, i);
		float fd = factor(i < 3 ? T->dst_rgb : T->dst_a, s, d, i);

		if (!T->enable)
			r[i] = s[i];
		else if (eq == GL_FUNC_ADD)
			r[i] = s[i] * fs + d[i] * fd;
		else if (eq == GL_FUNC_SUBTRACT)
			r[i] = s[i] * fs - d[i] * fd;
		else
			r[i] = d[i] * fd - s[i] * fs;
	}
	for (int i = 0; i < 4; i++)
		if (T->mask[i])
			d[i] = to8(r[i]) / 255.0f;
}

static const char *vs_src =
	"attribute vec4 p;\n"
	"varying vec2 st;\n"
	"void main() { st = p.xy * 0.5 + 0.5; gl_Position = p; }\n";
static const char *fs_bg =
	"precision highp float;\n"
	"varying vec2 st;\n"
	"void main() { gl_FragColor = vec4(st.x, st.y, 1.0 - st.x, 0.25 + 0.5 * st.y); }\n";
static const char *fs_src =
	"precision highp float;\n"
	"varying vec2 st;\n"
	"void main() { gl_FragColor = vec4(st.y, 0.5, st.x, st.x); }\n";

static GLuint build(const char *fs)
{
	const char *src[2] = { vs_src, fs };
	GLenum type[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	GLuint p = glCreateProgram();
	char log[1024];
	GLint ok;

	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(type[i]);

		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			glGetShaderInfoLog(s, sizeof(log), NULL, log);
			fprintf(stderr, "glblend: shader: %s\n", log);
			exit(2);
		}
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "p");
	glLinkProgram(p);
	return p;
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
	static float quad[2 * 24];
	static const float q1[] = { -1, -1, 0, 1,  1, -1, 0, 1,  1, 1, 0, 1,
	                            -1, -1, 0, 1,  1, 1, 0, 1,  -1, 1, 0, 1 };
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	uint8_t *px = malloc(W * H * 4);
	int run = 0, failed = 0;
	EGLDisplay dpy;
	EGLContext ctx;
	GLuint rt, fbo, pbg, psrc;

	memcpy(quad, q1, sizeof(q1));
	memcpy(quad + 24, q1, sizeof(q1));
	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glblend: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glblend: no GLES 2 context\n"), 2;

	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glViewport(0, 0, W, H);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, quad);
	pbg = build(fs_bg);
	psrc = build(fs_src);
	glBlendColor(CONST[0], CONST[1], CONST[2], CONST[3]);

	for (unsigned k = 0; k < sizeof(tests) / sizeof(tests[0]); k++) {
		const struct test *T = &tests[k];
		int worst = 0, wx = 0, wy = 0, got[4] = { 0 }, want[4] = { 0 };

		if (!wanted(argc, argv, T->name))
			continue;
		run++;
		glDisable(GL_BLEND);
		glColorMask(1, 1, 1, 1);
		glUseProgram(pbg);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glUseProgram(psrc);
		if (T->enable)
			glEnable(GL_BLEND);
		glBlendEquationSeparate(T->eq_rgb, T->eq_a);
		glBlendFuncSeparate(T->src_rgb, T->dst_rgb, T->src_a, T->dst_a);
		glColorMask(T->mask[0], T->mask[1], T->mask[2], T->mask[3]);
		glDrawArrays(GL_TRIANGLES, 0, T->twice ? 12 : 6);
		glColorMask(1, 1, 1, 1);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);

		for (int gy = 0; gy < GRID; gy++) {
			for (int gx = 0; gx < GRID; gx++) {
				int x = (gx * W + W / 2) / GRID, y = (gy * H + H / 2) / GRID;
				float s = (x + 0.5f) / W, t = (y + 0.5f) / H;
				float bg[4] = { s, t, 1 - s, 0.25f + 0.5f * t }, d[4];
				float src[4] = { t, 0.5f, s, s };
				const uint8_t *q = px + (y * W + x) * 4;

				for (int i = 0; i < 4; i++)
					d[i] = to8(bg[i]) / 255.0f;
				blend(T, src, d);
				if (T->twice)
					blend(T, src, d);
				for (int i = 0; i < 4; i++) {
					int e = abs(q[i] - to8(d[i]));

					if (e > worst) {
						worst = e, wx = x, wy = y;
						for (int j = 0; j < 4; j++)
							got[j] = q[j], want[j] = to8(d[j]);
					}
				}
			}
		}
		if (worst <= 2) {
			printf("%-14s ok (largest difference %d)\n", T->name, worst);
		} else {
			failed++;
			printf("%-14s WRONG by %d at %d,%d: %02x %02x %02x %02x, want %02x %02x %02x %02x\n",
			       T->name, worst, wx, wy, got[0], got[1], got[2], got[3],
			       want[0], want[1], want[2], want[3]);
		}
	}
	printf("%d of %d cases right\n", run - failed, run);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	free(px);
	return failed ? 1 : 0;
}
