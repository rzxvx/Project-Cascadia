/* glfs -- the fragment shader compiler's tests (docs/research/p105-mesa.md,
 * M13c): each case is a fragment shader and the same arithmetic in C.  A
 * full-screen quad carries two varyings,
 *
 *   v = (s, t, 1 - s, 1 - t)        s, t = 0..1 left to right, bottom to top
 *   w = (4s - 2, 4t - 2, s - t, 0.25)
 *
 * (affine: a varying is interpolated linearly over each triangle) and three
 * uniforms (u0, u1, k below); the shader's colour is read back
 * on a grid of pixels and checked against the C, clamped to 0..1 and
 * rounded as the 8-bit target has it.
 *
 *   sgx-gl glfs [CASE...]        (a name, or a prefix; all by default)
 *
 * Exit status 0 when every case passed.  A case whose shader the driver
 * could not compile is drawn the M13a way (sgx_draw.c), wrong but harmless;
 * Mesa says which ones (MESA: warning: sgx: a fragment shader not compiled).
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
#define GRID 24

static const float U0[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
static const float U1[4] = { -1.0f, 2.0f, 0.5f, 3.0f };
static const float K = 0.3f;

typedef void (*ref_fn)(const float v[4], const float w[4], float c[4]);

#define REF(name) static void name(const float v[4], const float w[4], float c[4])
#define SET(a, b, cc, d) do { c[0] = (a); c[1] = (b); c[2] = (cc); c[3] = (d); } while (0)

static float fract_(float x) { return x - floorf(x); }
static float step_(float e, float x) { return x < e ? 0.0f : 1.0f; }
static float clamp_(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static float smooth_(float a, float b, float x)
{
	float t = clamp_((x - a) / (b - a), 0, 1);
	return t * t * (3 - 2 * t);
}

REF(r_varying) { SET(v[0], v[1], v[2], v[3]); }
REF(r_second) { SET(w[2], w[3] * 2, v[0] * 0.5f, 1); }
REF(r_add) { for (int i = 0; i < 4; i++) c[i] = v[i] + w[i] * 0.25f; }
REF(r_mul) { SET(v[0] * v[1], v[1] * v[0], v[2] * v[3], v[3] * v[2]); }
REF(r_uniform) { for (int i = 0; i < 4; i++) c[i] = U0[i] * v[i]; }
REF(r_mad) { for (int i = 0; i < 4; i++) c[i] = v[i] * U1[i] + U0[i] * 0.5f; }
REF(r_scalar_uniform) { SET(K, v[0] * K, K + v[1] * 0.5f, 1); }
REF(r_minmax) { SET(fminf(v[0], v[1]), fmaxf(v[0], v[1]), fminf(w[0], 0.5f), fmaxf(w[1], 0.2f)); }
REF(r_clamp) { for (int i = 0; i < 4; i++) c[i] = clamp_(w[i] * 0.5f + 0.5f, 0.2f, 0.7f); }
REF(r_fract) { SET(fract_(w[0]), floorf(w[1]) * 0.25f + 0.5f, fract_(v[0] * 3), ceilf(w[0]) * 0.2f + 0.5f); }
REF(r_div) { SET(0.1f / (v[0] + 0.1f), v[1] / (v[0] + 1), 1 / (w[2] * 0.5f + 1.5f), 1); }
REF(r_sqrt) { SET(sqrtf(v[0]), 1 / sqrtf(v[1] + 1), sqrtf(v[0] * v[1]), 1); }
REF(r_exp) { SET(exp2f(-v[0] * 3), log2f(v[1] + 1), expf(v[0]) * 0.3f, logf(v[1] * 2 + 1) * 0.5f); }
REF(r_pow) { SET(powf(v[0], 2.2f), powf(v[1], 0.45f), powf(v[0] + 0.5f, 3) * 0.2f, 1); }
REF(r_trig) { SET(sinf(v[0] * 6.2831853f) * 0.5f + 0.5f, cosf(v[1] * 6.2831853f) * 0.5f + 0.5f, sinf(w[0]) * 0.5f + 0.5f, 1); }
REF(r_dot)
{
	float l = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
	SET(v[0] * U0[0] + v[1] * U0[1] + v[2] * U0[2], sqrtf(v[0] * v[0] + v[1] * v[1]) * 0.5f,
	    (l > 0 ? w[0] / l : 0) * 0.5f + 0.5f, 1);
}
REF(r_compare) { SET(v[0] < v[1] ? 1.0f : 0.0f, v[0] >= 0.5f ? 0.25f : 0.75f, w[0] > 0.0f ? 1.0f : 0.5f, step_(0.3f, v[1])); }
REF(r_mix) { SET(v[0] * 0.7f + v[1] * 0.3f, smooth_(0.2f, 0.8f, v[0]), U0[0] + (U0[3] - U0[0]) * v[1], 1); }
REF(r_abs) { SET(fabsf(w[0]) * 0.5f, (w[1] > 0 ? 1.0f : w[1] < 0 ? -1.0f : 0.0f) * 0.5f + 0.5f, -w[0] * 0.25f + 0.5f, fabsf(-v[1])); }
REF(r_if) { if (v[0] > 0.5f) SET(1, 0, 0, 1); else SET(0, v[1], 0, 1); }
REF(r_loop) { for (int i = 0; i < 4; i++) c[i] = v[i] * 0.2f * 4; }
REF(r_saturate) { SET(2, -1, v[0] * 3, 1); }
REF(r_long)
{
	float a = v[0] * v[1], b = v[2] + w[2], d = a * b - v[3], e = d * d + 0.1f;
	float f = e * a + b * d, g = f * 0.5f + e * 0.25f, h = g - a * 0.5f + b * 0.125f;
	SET(fract_(h + g), fract_(a + b + d), fract_(e * 3 + f), fract_(g * h + 0.5f));
}

static const struct test {
	const char *name, *body;
	ref_fn ref;
	int tol;        /* in 8-bit steps */
} tests[] = {
	{ "varying", "c = v;", r_varying, 1 },
	{ "second", "c = vec4(w.z, w.w * 2.0, v.x * 0.5, 1.0);", r_second, 1 },
	{ "add", "c = v + w * 0.25;", r_add, 1 },
	{ "mul", "c = v * v.yxwz;", r_mul, 1 },
	{ "uniform", "c = u0 * v;", r_uniform, 1 },
	{ "mad", "c = v * u1 + u0 * 0.5;", r_mad, 1 },
	{ "scalar_uniform", "c = vec4(k, v.x * k, k + v.y * 0.5, 1.0);", r_scalar_uniform, 1 },
	{ "minmax", "c = vec4(min(v.x, v.y), max(v.x, v.y), min(w.x, 0.5), max(w.y, 0.2));", r_minmax, 1 },
	{ "clamp", "c = clamp(w * 0.5 + 0.5, 0.2, 0.7);", r_clamp, 1 },
	{ "fract", "c = vec4(fract(w.x), floor(w.y) * 0.25 + 0.5, fract(v.x * 3.0), ceil(w.x) * 0.2 + 0.5);", r_fract, 1 },
	{ "div", "c = vec4(0.1 / (v.x + 0.1), v.y / (v.x + 1.0), 1.0 / (w.z * 0.5 + 1.5), 1.0);", r_div, 1 },
	{ "sqrt", "c = vec4(sqrt(v.x), inversesqrt(v.y + 1.0), sqrt(v.x * v.y), 1.0);", r_sqrt, 1 },
	{ "exp", "c = vec4(exp2(-v.x * 3.0), log2(v.y + 1.0), exp(v.x) * 0.3, log(v.y * 2.0 + 1.0) * 0.5);", r_exp, 2 },
	{ "pow", "c = vec4(pow(v.x, 2.2), pow(v.y, 0.45), pow(v.x + 0.5, 3.0) * 0.2, 1.0);", r_pow, 2 },
	{ "trig", "c = vec4(sin(v.x * 6.2831853) * 0.5 + 0.5, cos(v.y * 6.2831853) * 0.5 + 0.5, sin(w.x) * 0.5 + 0.5, 1.0);", r_trig, 2 },
	{ "dot", "c = vec4(dot(v.xyz, u0.xyz), length(v.xy) * 0.5, normalize(w.xyz).x * 0.5 + 0.5, 1.0);", r_dot, 1 },
	{ "compare", "c = vec4(v.x < v.y ? 1.0 : 0.0, v.x >= 0.5 ? 0.25 : 0.75, w.x > 0.0 ? 1.0 : 0.5, step(0.3, v.y));", r_compare, 1 },
	{ "mix", "c = vec4(mix(v.x, v.y, 0.3), smoothstep(0.2, 0.8, v.x), mix(u0.x, u0.w, v.y), 1.0);", r_mix, 1 },
	{ "abs", "c = vec4(abs(w.x) * 0.5, sign(w.y) * 0.5 + 0.5, -w.x * 0.25 + 0.5, abs(-v.y));", r_abs, 1 },
	{ "if", "if (v.x > 0.5) c = vec4(1.0, 0.0, 0.0, 1.0); else c = vec4(0.0, v.y, 0.0, 1.0);", r_if, 1 },
	{ "loop", "c = vec4(0.0); for (int i = 0; i < 4; i++) c += v * 0.2;", r_loop, 1 },
	{ "saturate", "c = vec4(2.0, -1.0, v.x * 3.0, 1.0);", r_saturate, 1 },
	{ "long", "float a = v.x * v.y, b = v.z + w.z, d = a * b - v.w, e = d * d + 0.1;"
	          "float f = e * a + b * d, g = f * 0.5 + e * 0.25, h = g - a * 0.5 + b * 0.125;"
	          "c = vec4(fract(h + g), fract(a + b + d), fract(e * 3.0 + f), fract(g * h + 0.5));", r_long, 2 },
};

static const char *vs_src =
	"attribute vec4 p;\n"
	"varying vec4 v, w;\n"
	"void main() {\n"
	"  vec2 st = p.xy * 0.5 + 0.5;\n"
	"  v = vec4(st, 1.0 - st);\n"
	"  w = vec4(st * 4.0 - 2.0, st.x - st.y, 0.25);\n"
	"  gl_Position = p;\n"
	"}\n";

static GLuint build(const char *body)
{
	static char fs[4096];
	const char *src[2] = { vs_src, fs };
	GLenum type[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	GLuint p = glCreateProgram();
	char log[1024];
	GLint ok;

	snprintf(fs, sizeof(fs),
		 "precision highp float;\n"
		 "varying vec4 v, w;\n"
		 "uniform vec4 u0, u1;\n"
		 "uniform float k;\n"
		 "void main() { vec4 c; %s gl_FragColor = c; }\n", body);
	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(type[i]);

		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			glGetShaderInfoLog(s, sizeof(log), NULL, log);
			fprintf(stderr, "glfs: shader: %s\n", log);
			return 0;
		}
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "p");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		glGetProgramInfoLog(p, sizeof(log), NULL, log);
		fprintf(stderr, "glfs: link: %s\n", log);
		return 0;
	}
	return p;
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
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	uint8_t *px = malloc(W * H * 4);
	int run = 0, failed = 0;
	EGLDisplay dpy;
	EGLContext ctx;
	GLuint tex, fbo;

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glfs: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glfs: no GLES 2 context\n"), 2;

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	glViewport(0, 0, W, H);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, quad);

	for (unsigned t = 0; t < sizeof(tests) / sizeof(tests[0]); t++) {
		const struct test *T = &tests[t];
		int worst = 0, wx = 0, wy = 0, got[4] = { 0 }, want[4] = { 0 };
		GLuint p;

		if (!wanted(argc, argv, T->name))
			continue;
		run++;
		if (!(p = build(T->body))) {
			printf("%-16s does not build\n", T->name);
			failed++;
			continue;
		}
		glUseProgram(p);
		glUniform4fv(glGetUniformLocation(p, "u0"), 1, U0);
		glUniform4fv(glGetUniformLocation(p, "u1"), 1, U1);
		glUniform1f(glGetUniformLocation(p, "k"), K);
		glClearColor(0, 0, 0, 0);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);

		for (int gy = 0; gy < GRID; gy++) {
			for (int gx = 0; gx < GRID; gx++) {
				int x = (gx * W + W / 2) / GRID, y = (gy * H + H / 2) / GRID;
				float s = (x + 0.5f) / W, tt = (y + 0.5f) / H;
				float v[4] = { s, tt, 1 - s, 1 - tt };
				float w[4] = { 4 * s - 2, 4 * tt - 2, s - tt, 0.25f }, c[4];
				const uint8_t *q = px + (y * W + x) * 4;

				T->ref(v, w, c);
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
		if (worst <= T->tol) {
			printf("%-16s ok (largest difference %d)\n", T->name, worst);
		} else {
			failed++;
			printf("%-16s WRONG by %d at %d,%d: %02x %02x %02x %02x, want %02x %02x %02x %02x\n",
			       T->name, worst, wx, wy, got[0], got[1], got[2], got[3],
			       want[0], want[1], want[2], want[3]);
		}
		glDeleteProgram(p);
	}
	printf("%d of %d cases right\n", run - failed, run);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	free(px);
	return failed ? 1 : 0;
}
