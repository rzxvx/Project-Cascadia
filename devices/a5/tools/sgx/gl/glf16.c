/* glf16 -- colours worked out in F16 (docs/research/p105-mesa.md, M46): the
 * fragment programs an emulator makes (PPSSPP's texture times colour,
 * doubled; its fog; its stencil value as alpha) and the shapes around them
 * -- constants, uniforms, negation, min and max, swizzles, channels worked
 * out each its own way -- over a gradient, every pixel against the same in
 * C, to within 3 of 255 (F16's rounding, the varyings as F16).
 *
 *   sgx-gl glf16 [CASE]     (a name's start; all by default)
 *
 * With SGX_DEBUG_SHADER=1 the driver prints each program (F16 ones say so:
 * `SGX_DEBUG_DRAW=1` too).  Exit status 0 when every case passed. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 48
#define TS 16

static const float FOG[3] = { 0.2f, 0.5f, 0.8f }, STENCIL = 0.6f, K[4] = { 0.3f, 0.6f, 1.5f, 0.9f };

/* the varyings at a pixel, as the vertex shader below makes them */
struct px {
	float c[4], uv[2], fog, t[4];
};

typedef void (*ref_fn)(const struct px *p, float out[4]);

static float sat(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }
static float mixf(float a, float b, float t) { return a + (b - a) * t; }

static void r_plain(const struct px *p, float o[4])
{
	for (int i = 0; i < 4; i++)
		o[i] = p->c[i] * p->t[i];
}

static void r_doubled(const struct px *p, float o[4])
{
	for (int i = 0; i < 4; i++)
		o[i] = p->c[i] * p->t[i];
	for (int i = 0; i < 3; i++)
		o[i] = sat(o[i] * 2);
}

static void r_fog(const struct px *p, float o[4])
{
	float v[4], f = sat(p->fog);

	r_doubled(p, v);
	for (int i = 0; i < 3; i++)
		o[i] = mixf(FOG[i], v[i], f);
	o[3] = v[3];
}

static void r_stencil(const struct px *p, float o[4])
{
	float v[4], f = sat(p->fog);

	for (int i = 0; i < 3; i++)
		v[i] = sat(p->c[i] * p->t[i] * 2);
	for (int i = 0; i < 3; i++)
		o[i] = mixf(FOG[i], v[i], f);
	o[3] = STENCIL;
}

static void r_alpha1(const struct px *p, float o[4])
{
	for (int i = 0; i < 3; i++)
		o[i] = p->c[i] * p->t[i];
	o[3] = 1;
}

static void r_consts(const struct px *p, float o[4])
{
	static const float m[4] = { 0.25f, 0.5f, 0.75f, 1.0f }, a[4] = { 0.1f, 0, 0.05f, 0 };

	for (int i = 0; i < 4; i++)
		o[i] = p->c[i] * m[i] + a[i];
}

static void r_inverse(const struct px *p, float o[4])
{
	for (int i = 0; i < 4; i++)
		o[i] = 1 - p->c[i];
}

static void r_minmax(const struct px *p, float o[4])
{
	for (int i = 0; i < 4; i++)
		o[i] = fmaxf(p->c[i], p->t[i]) * fminf(p->c[i], 0.5f);
}

static void r_lanes(const struct px *p, float o[4])
{
	for (int i = 0; i < 3; i++)
		o[i] = sat(p->c[i] * 2);
	o[3] = p->c[3] * p->t[3] + 0.25f;
}

static void r_swizzled(const struct px *p, float o[4])
{
	static const int a[4] = { 2, 1, 0, 3 }, b[4] = { 1, 2, 3, 0 };

	for (int i = 0; i < 4; i++)
		o[i] = p->c[a[i]] * p->t[b[i]];
}

static void r_uniform(const struct px *p, float o[4])
{
	for (int i = 0; i < 4; i++)
		o[i] = p->c[i] * K[i];
}

static void r_halves(const struct px *p, float o[4])
{
	for (int i = 0; i < 4; i++)
		o[i] = p->c[i] * 0.5f + p->t[i] * 0.5f;
}

static void r_broadcast(const struct px *p, float o[4])
{
	for (int i = 0; i < 4; i++)
		o[i] = p->c[i] * p->fog;
}

static const struct test {
	const char *name, *body;
	ref_fn ref;
} tests[] = {
	{ "plain", "gl_FragColor = v_color0 * texture2D(tex, v_texcoord);", r_plain },
	{ "doubled", "vec4 v = v_color0 * texture2D(tex, v_texcoord);\n"
	             "v.rgb = clamp(v.rgb * 2.0, 0.0, 1.0); gl_FragColor = v;", r_doubled },
	{ "fog", "vec4 v = v_color0 * texture2D(tex, v_texcoord);\n"
	         "v.rgb = clamp(v.rgb * 2.0, 0.0, 1.0);\n"
	         "float f = clamp(v_fogdepth, 0.0, 1.0);\n"
	         "gl_FragColor = mix(vec4(u_fogcolor, v.a), v, f);", r_fog },
	{ "fog-stencil", "vec4 t = texture2D(tex, v_texcoord);\n"
	                 "vec4 v = v_color0 * vec4(t.rgb, 1.0);\n"
	                 "v.rgb = clamp(v.rgb * 2.0, 0.0, 1.0);\n"
	                 "float f = clamp(v_fogdepth, 0.0, 1.0);\n"
	                 "v = mix(vec4(u_fogcolor, v.a), v, f);\n"
	                 "gl_FragColor = vec4(v.rgb, u_stencil);", r_stencil },
	{ "alpha-one", "vec4 t = texture2D(tex, v_texcoord);\n"
	               "gl_FragColor = vec4(v_color0.rgb * t.rgb, 1.0);", r_alpha1 },
	{ "constants", "gl_FragColor = v_color0 * vec4(0.25, 0.5, 0.75, 1.0) + vec4(0.1, 0.0, 0.05, 0.0);",
	  r_consts },
	{ "inverse", "gl_FragColor = vec4(1.0) - v_color0;", r_inverse },
	{ "minmax", "vec4 t = texture2D(tex, v_texcoord);\n"
	            "gl_FragColor = max(v_color0, t) * min(v_color0, vec4(0.5));", r_minmax },
	{ "lanes", "vec4 t = texture2D(tex, v_texcoord);\n"
	           "gl_FragColor = vec4(clamp(v_color0.rgb * 2.0, 0.0, 1.0), v_color0.a * t.a + 0.25);",
	  r_lanes },
	{ "swizzled", "gl_FragColor = v_color0.bgra * texture2D(tex, v_texcoord).gbar;", r_swizzled },
	{ "uniform", "gl_FragColor = v_color0 * u_k;", r_uniform },
	{ "halves", "gl_FragColor = v_color0 * 0.5 + texture2D(tex, v_texcoord) * 0.5;", r_halves },
	{ "broadcast", "gl_FragColor = v_color0 * v_fogdepth;", r_broadcast },
};

static const char *vs_src =
	"attribute vec4 p;\n"
	"varying vec4 v_color0;\n"
	"varying vec2 v_texcoord;\n"
	"varying float v_fogdepth;\n"
	"void main() {\n"
	"  vec2 st = p.xy * 0.5 + 0.5;\n"
	"  gl_Position = p;\n"
	"  v_color0 = vec4(st.x, st.y, 1.0 - st.x, 0.25 + 0.5 * st.y);\n"
	"  v_texcoord = st;\n"
	"  v_fogdepth = 1.5 * st.x - 0.25;\n"
	"}\n";

static uint8_t texels[TS * TS * 4];

static void varyings(int x, int y, struct px *p)
{
	float s = (x + 0.5f) / W, t = (y + 0.5f) / H;
	int tx = (int)(s * TS), ty = (int)(t * TS);

	p->c[0] = s;
	p->c[1] = t;
	p->c[2] = 1 - s;
	p->c[3] = 0.25f + 0.5f * t;
	p->uv[0] = s;
	p->uv[1] = t;
	p->fog = 1.5f * s - 0.25f;
	for (int i = 0; i < 4; i++)
		p->t[i] = texels[4 * (ty * TS + tx) + i] / 255.0f;
}

static GLuint build(const char *body)
{
	static char fs[2048];
	const char *src[2] = { vs_src, fs };
	GLenum type[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	GLuint p = glCreateProgram();
	GLint ok;

	snprintf(fs, sizeof(fs),
	         "precision highp float;\n"
	         "uniform sampler2D tex;\n"
	         "uniform vec3 u_fogcolor;\n"
	         "uniform float u_stencil;\n"
	         "uniform vec4 u_k;\n"
	         "varying vec4 v_color0;\n"
	         "varying vec2 v_texcoord;\n"
	         "varying float v_fogdepth;\n"
	         "void main() {\n%s\n}\n", body);
	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(type[i]);

		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			char log[1024];

			glGetShaderInfoLog(s, sizeof(log), NULL, log);
			fprintf(stderr, "glf16: %s\n%s\n", log, i ? fs : vs_src);
			exit(2);
		}
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "p");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok)
		return fprintf(stderr, "glf16: a program did not link\n"), exit(2), 0;
	glUseProgram(p);
	glUniform1i(glGetUniformLocation(p, "tex"), 0);
	glUniform3fv(glGetUniformLocation(p, "u_fogcolor"), 1, FOG);
	glUniform1f(glGetUniformLocation(p, "u_stencil"), STENCIL);
	glUniform4fv(glGetUniformLocation(p, "u_k"), 1, K);
	return p;
}

int main(int argc, char **argv)
{
	static const float quad[] = { -1, -1, 0, 1, 1, -1, 0, 1, 1, 1, 0, 1,
	                              -1, -1, 0, 1, 1, 1, 0, 1, -1, 1, 0, 1 };
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	static uint8_t px[W * H * 4];
	int run = 0, failed = 0;
	EGLDisplay dpy;
	EGLContext ctx;
	GLuint rt, fbo, tex;

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glf16: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glf16: no GLES 2 context\n"), 2;

	for (int i = 0; i < TS * TS; i++) {
		texels[4 * i] = (i * 37) & 255;
		texels[4 * i + 1] = (i * 11 + 40) & 255;
		texels[4 * i + 2] = 255 - ((i * 23) & 255);
		texels[4 * i + 3] = 96 + (i * 7) % 160;
	}
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TS, TS, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glViewport(0, 0, W, H);
	glBindTexture(GL_TEXTURE_2D, tex);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, quad);

	for (unsigned k = 0; k < sizeof(tests) / sizeof(tests[0]); k++) {
		const struct test *T = &tests[k];
		int worst = 0, wx = 0, wy = 0, wc = 0, got = 0, want = 0;

		if (argc > 1 && strncmp(T->name, argv[1], strlen(argv[1])))
			continue;
		run++;
		build(T->body);
		glClearColor(0, 0, 0, 0);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		for (int y = 0; y < H; y++)
			for (int x = 0; x < W; x++) {
				struct px p;
				float o[4];

				varyings(x, y, &p);
				T->ref(&p, o);
				for (int c = 0; c < 4; c++) {
					int r = (int)(sat(o[c]) * 255 + 0.5f), g = px[4 * (y * W + x) + c];

					if (abs(r - g) > worst)
						worst = abs(r - g), wx = x, wy = y, wc = c, got = g, want = r;
				}
			}
		if (worst > 3) {
			failed++;
			printf("%-12s WRONG: off by %d at %d,%d channel %d (%d, wanted %d)\n", T->name, worst,
			       wx, wy, wc, got, want);
		} else {
			printf("%-12s ok (largest difference %d)\n", T->name, worst);
		}
	}
	printf("%d of %d cases right\n", run - failed, run);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return failed != 0;
}
