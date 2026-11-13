/* glvsmath -- a vertex shader's arithmetic (docs/research/p105-mesa.md,
 * M46): what PPSSPP's vertex shaders do -- matrices times vectors either
 * way round, products and sums (FMAD: fused in vertex shaders), clamps and
 * the constants 0, 1, 2 and 0.5 either sign (V32NMAD's src1 swizzle), a
 * light's dot product -- worked out in the vertex shader for each of 64
 * quads whose four vertices share their inputs, the result a varying the
 * same over the quad, each quad's centre read back against the same in C
 * (within 2 of 255: the result is mapped into 0..1 first).
 *
 *   sgx-gl glvsmath [CASE]     (a name's start; all by default)
 *
 * Exit status 0 when every case passed. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 8             /* quads a side */
#define Q 8             /* pixels a quad side */
#define W (N * Q)
#define H (N * Q)

/* the uniforms: m a matrix (column-major, as GL takes it), u and v vectors */
static const float M[16] = { 0.25f, -0.5f, 0.75f, 0.1f,  0.6f, 0.2f, -0.3f, 0.05f,
                             -0.4f, 0.35f, 0.15f, -0.2f, 0.3f, -0.1f, 0.45f, 0.9f };
static const float U[4] = { 0.7f, -0.35f, 1.25f, 0.4f }, V[4] = { -0.6f, 0.8f, 0.15f, -1.1f };

/* a quad's inputs: a (vec4) and b (vec4) */
struct in {
	float a[4], b[4];
};

typedef void (*ref_fn)(const struct in *p, float o[4]);

static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

/* m * a (columns times a's components) */
static void mat_vec(const float *m, const float *a, float o[4])
{
	for (int r = 0; r < 4; r++)
		o[r] = m[r] * a[0] + m[4 + r] * a[1] + m[8 + r] * a[2] + m[12 + r] * a[3];
}

/* a * m (a dotted with each column) */
static void vec_mat(const float *a, const float *m, float o[4])
{
	for (int col = 0; col < 4; col++)
		o[col] = a[0] * m[4 * col] + a[1] * m[4 * col + 1] + a[2] * m[4 * col + 2] +
		         a[3] * m[4 * col + 3];
}

static void r_mat_vec(const struct in *p, float o[4]) { mat_vec(M, p->a, o); }
static void r_vec_mat(const struct in *p, float o[4]) { vec_mat(p->a, M, o); }

/* PPSSPP's: mul(vec4(position, 1.0), u_world).xyz, then times u_proj */
static void r_world_proj(const struct in *p, float o[4])
{
	float a[4] = { p->a[0], p->a[1], p->a[2], 1.0f }, w[4], t[4];

	vec_mat(a, M, w);
	t[0] = w[0]; t[1] = w[1]; t[2] = w[2]; t[3] = 1.0f;
	mat_vec(M, t, o);
}

static void r_mad(const struct in *p, float o[4])
{
	for (int i = 0; i < 4; i++)
		o[i] = p->a[i] * p->b[i] + U[i];
}

static void r_mad_neg(const struct in *p, float o[4])
{
	o[0] = -p->a[0] * p->b[0] + U[0];
	o[1] = p->a[1] * -p->b[1] - U[1];
	o[2] = -(p->a[2] * U[2]) + p->b[2];
	o[3] = fabsf(p->a[3]) * p->b[3] - fabsf(U[3]);
}

static void r_mad_uniforms(const struct in *p, float o[4])
{
	for (int i = 0; i < 4; i++)
		o[i] = U[i] * V[i] + p->a[i];
}

static void r_clamp(const struct in *p, float o[4])
{
	o[0] = clampf(p->a[0] * 2.0f, 0.0f, 1.0f);
	o[1] = clampf(p->a[1], 0.0f, 1.0f);
	o[2] = clampf(-p->a[2], 0.0f, 1.0f);
	o[3] = clampf(fabsf(p->a[3]) - 0.5f, 0.0f, 1.0f);
}

static void r_consts(const struct in *p, float o[4])
{
	o[0] = p->a[0] * -1.0f + 0.5f;
	o[1] = p->a[1] * 0.5f - 2.0f;
	o[2] = fmaxf(p->a[2], 0.0f) + fminf(p->b[2], 1.0f) * -0.5f;
	o[3] = (-p->a[3]) * 2.0f + 1.0f;
}

static void r_light(const struct in *p, float o[4])
{
	float n[3] = { p->a[0], p->a[1], p->a[2] }, l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	float d;

	for (int i = 0; i < 3; i++)
		n[i] /= l;
	d = fmaxf(n[0] * U[0] + n[1] * U[1] + n[2] * U[2], 0.0f);
	for (int i = 0; i < 3; i++)
		o[i] = clampf(V[i] * p->b[i] + U[i] * p->b[i] * d, 0.0f, 1.0f);
	o[3] = p->b[3];
}

static const struct test {
	const char *name, *expr;    /* the vertex shader's r */
	ref_fn ref;
	float scale, bias;          /* the colour: r * scale + bias */
} tests[] = {
	{ "mat-vec", "r = m * a;", r_mat_vec, 0.25f, 0.5f },
	{ "vec-mat", "r = a * m;", r_vec_mat, 0.25f, 0.5f },
	{ "world-proj", "vec3 w = (vec4(a.xyz, 1.0) * m).xyz; r = m * vec4(w, 1.0);", r_world_proj,
	  0.2f, 0.5f },
	{ "mad", "r = a * b + u;", r_mad, 0.2f, 0.5f },
	{ "mad-neg", "r = vec4(-a.x * b.x + u.x, a.y * -b.y - u.y, -(a.z * u.z) + b.z, "
	             "abs(a.w) * b.w - abs(u.w));", r_mad_neg, 0.2f, 0.5f },
	{ "mad-uniforms", "r = u * v + a;", r_mad_uniforms, 0.2f, 0.5f },
	{ "clamp", "r = vec4(clamp(a.x * 2.0, 0.0, 1.0), clamp(a.y, 0.0, 1.0), clamp(-a.z, 0.0, 1.0), "
	           "clamp(abs(a.w) - 0.5, 0.0, 1.0));", r_clamp, 1.0f, 0.0f },
	{ "consts", "r = vec4(a.x * -1.0 + 0.5, a.y * 0.5 - 2.0, max(a.z, 0.0) + min(b.z, 1.0) * -0.5, "
	            "(-a.w) * 2.0 + 1.0);", r_consts, 0.2f, 0.5f },
	{ "light", "vec3 n = normalize(a.xyz); float d = max(dot(n, u.xyz), 0.0); "
	           "r = vec4(clamp(v.xyz * b.xyz + u.xyz * b.xyz * d, 0.0, 1.0), b.w);", r_light, 1.0f,
	  0.0f },
};

static GLuint build(const struct test *T)
{
	static char vs[2048];
	static const char *fs = "precision highp float; varying vec4 c;\n"
	                        "void main() { gl_FragColor = c; }\n";
	const char *src[2] = { vs, fs };
	GLenum type[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	GLuint p = glCreateProgram();
	GLint ok;

	snprintf(vs, sizeof(vs),
	         "attribute vec2 pos; attribute vec4 a; attribute vec4 b;\n"
	         "uniform mat4 m; uniform vec4 u; uniform vec4 v; uniform vec2 sb;\n"
	         "varying vec4 c;\n"
	         "void main() {\n"
	         "  vec4 r;\n"
	         "  %s\n"
	         "  c = r * sb.x + sb.y;\n"
	         "  gl_Position = vec4(pos, 0.0, 1.0);\n"
	         "}\n", T->expr);
	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(type[i]);

		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			char log[1024];

			glGetShaderInfoLog(s, sizeof(log), NULL, log);
			fprintf(stderr, "glvsmath: %s\n%s\n", log, src[i]);
			exit(2);
		}
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "pos");
	glBindAttribLocation(p, 1, "a");
	glBindAttribLocation(p, 2, "b");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok)
		return fprintf(stderr, "glvsmath: a program did not link\n"), exit(2), 0;
	glUseProgram(p);
	glUniformMatrix4fv(glGetUniformLocation(p, "m"), 1, GL_FALSE, M);
	glUniform4fv(glGetUniformLocation(p, "u"), 1, U);
	glUniform4fv(glGetUniformLocation(p, "v"), 1, V);
	glUniform2f(glGetUniformLocation(p, "sb"), T->scale, T->bias);
	return p;
}

/* quad k's inputs: a spread of values, signs both ways */
static void inputs(int k, struct in *p)
{
	for (int i = 0; i < 4; i++) {
		p->a[i] = sinf(k * 1.7f + i * 2.3f) * 1.6f;
		p->b[i] = cosf(k * 0.9f + i * 1.1f) * 1.2f;
	}
	/* (a normal never 0: the light case normalizes it) */
	p->a[2] += p->a[2] < 0 ? -0.2f : 0.2f;
}

int main(int argc, char **argv)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	static float verts[N * N * 6 * 10];
	static uint8_t px[W * H * 4];
	int run = 0, failed = 0;
	EGLDisplay dpy;
	EGLContext ctx;
	GLuint rt, fbo, vbo;

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glvsmath: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glvsmath: no context\n"), 2;
	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glViewport(0, 0, W, H);

	/* the quads: two triangles each, x, y, a, b at every vertex */
	for (int k = 0; k < N * N; k++) {
		static const float corner[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };
		struct in p;

		inputs(k, &p);
		for (int v = 0; v < 6; v++) {
			float *o = &verts[(k * 6 + v) * 10];

			o[0] = ((k % N) + corner[v][0]) * 2.0f / N - 1.0f;
			o[1] = ((k / N) + corner[v][1]) * 2.0f / N - 1.0f;
			memcpy(o + 2, p.a, 16);
			memcpy(o + 6, p.b, 16);
		}
	}
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
	for (int i = 0; i < 3; i++)
		glEnableVertexAttribArray(i);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 40, (void *)0);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 40, (void *)8);
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 40, (void *)24);

	for (unsigned t = 0; t < sizeof(tests) / sizeof(tests[0]); t++) {
		const struct test *T = &tests[t];
		int worst = 0, wk = 0, wc = 0, got = 0, want = 0;
		GLuint prog;

		if (argc > 1 && strncmp(T->name, argv[1], strlen(argv[1])))
			continue;
		run++;
		prog = build(T);
		glClearColor(0, 0, 0, 0);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, N * N * 6);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		glDeleteProgram(prog);
		for (int k = 0; k < N * N; k++) {
			int x = (k % N) * Q + Q / 2, y = (k / N) * Q + Q / 2;
			struct in p;
			float o[4];

			inputs(k, &p);
			T->ref(&p, o);
			for (int c = 0; c < 4; c++) {
				int r = (int)(clampf(o[c] * T->scale + T->bias, 0.0f, 1.0f) * 255 + 0.5f);
				int g = px[4 * (y * W + x) + c];

				if (abs(r - g) > worst)
					worst = abs(r - g), wk = k, wc = c, got = g, want = r;
			}
		}
		if (worst > 2) {
			failed++;
			printf("%-13s WRONG: off by %d at quad %d channel %d (%d, wanted %d)\n", T->name,
			       worst, wk, wc, got, want);
		} else {
			printf("%-13s ok (largest difference %d)\n", T->name, worst);
		}
		fflush(stdout);
	}
	printf("%d of %d cases right\n", run - failed, run);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return failed != 0;
}
