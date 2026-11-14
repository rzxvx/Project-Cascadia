/* gltexclamp -- PPSSPP's clamped lookups left to the PDS where the clamp
 * does nothing (docs/research/p105-mesa.md, M46): a lookup at clamp(uv,
 * u_texclamp.zw, u_texclamp.xy - u_texclamp.zw) + u_texclampoff, as
 * PPSSPP's TClampST programs have it, uv an attribute the vertex shader
 * copies (PPSSPP's through mode), drawn indexed from buffers over a quad
 * whose uv runs over a range -- every pixel against the same program
 * drawn with the clamp kept (SGX_NO_TEXCLAMP_FREE=1 while it is made and
 * drawn: the GPU's own lookups, within 1 of 255 for linear's), and against
 * the same clamp and lookup in C (nearest: exact, a coordinate on a
 * texel's edge either texel; linear: within 12 -- the texture unit's
 * filter weights are coarser than C's, 4 to 7 off on these texels).
 *
 * The driver leaves the clamp out when the offset is 0 and uv stays inside
 * the rectangle at every vertex, or leaves it only at the texture's own
 * edge with the sampler clamping to the edge; each case says which it
 * should be, and SGX_DEBUG_TEXCLAMP=1 has the driver say which it was.
 * The texture 16 x 16, or 24 x 20 (the npot cases: the sampler's edge is
 * the texture's, not its copy's padded to powers of two) -- uploaded, or
 * (rt) rendered by the GPU first, as God of War's effects sample what they
 * rendered.
 *
 *   sgx-gl gltexclamp [CASE]     (a name's start; all by default)
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

#define W 64
#define H 48

static uint8_t texels[24 * 20 * 4];
static int tw, th;      /* the case's texture's size */

static const struct test {
	const char *name;
	float clamp[4], off[2];
	float uv0[2], uv1[2];   /* uv at the bottom left, the top right */
	GLenum filter, wrap;
	int mip;                /* mipmaps, sampled LINEAR_MIPMAP_NEAREST */
	int scaled;             /* the vertex shader scales uv (by 1): not a copy */
	const char *should;     /* the clamp left out or kept */
	int npot;               /* 24 x 20, not 16 x 16; 2: rendered by the GPU */
} tests[] = {
	{ "inside", { 0.75f, 0.625f, 1.0f / 32, 1.0f / 32 }, { 0, 0 }, { 0.1f, 0.1f }, { 0.6f, 0.55f },
	  GL_NEAREST, GL_CLAMP_TO_EDGE, 0, 0, "left out" },
	{ "inside-lin", { 0.75f, 0.625f, 1.0f / 32, 1.0f / 32 }, { 0, 0 }, { 0.1f, 0.1f },
	  { 0.6f, 0.55f }, GL_LINEAR, GL_CLAMP_TO_EDGE, 0, 0, "left out" },
	{ "inside-rep", { 0.75f, 0.625f, 1.0f / 32, 1.0f / 32 }, { 0, 0 }, { 0.1f, 0.1f },
	  { 0.6f, 0.55f }, GL_LINEAR, GL_REPEAT, 0, 0, "left out" },
	{ "outside", { 0.75f, 0.625f, 1.0f / 32, 1.0f / 32 }, { 0, 0 }, { 0, 0 }, { 1, 1 },
	  GL_NEAREST, GL_CLAMP_TO_EDGE, 0, 0, "kept" },
	{ "outside-lin", { 0.75f, 0.625f, 1.0f / 32, 1.0f / 32 }, { 0, 0 }, { 0, 0 }, { 1, 1 },
	  GL_LINEAR, GL_CLAMP_TO_EDGE, 0, 0, "kept" },
	{ "edge", { 1, 1, 1.0f / 32, 1.0f / 32 }, { 0, 0 }, { -0.25f, -0.25f }, { 1.25f, 1.25f },
	  GL_NEAREST, GL_CLAMP_TO_EDGE, 0, 0, "left out" },
	{ "edge-lin", { 1, 1, 1.0f / 32, 1.0f / 32 }, { 0, 0 }, { -0.25f, -0.25f }, { 1.25f, 1.25f },
	  GL_LINEAR, GL_CLAMP_TO_EDGE, 0, 0, "left out" },
	{ "edge-rep", { 1, 1, 1.0f / 32, 1.0f / 32 }, { 0, 0 }, { -0.25f, -0.25f }, { 1.25f, 1.25f },
	  GL_NEAREST, GL_REPEAT, 0, 0, "kept" },
	/* (God of War's: the PSP's texture larger than the one it rendered) */
	{ "edge-wide", { 512.0f / 480, 512.0f / 272, 1.0f / 32, 1.0f / 32 }, { 0, 0 }, { 0, 0 },
	  { 1, 1 }, GL_LINEAR, GL_CLAMP_TO_EDGE, 0, 0, "left out" },
	{ "one-side", { 0.75f, 0.625f, 1.0f / 32, 1.0f / 32 }, { 0, 0 }, { -0.2f, 0.1f },
	  { 0.6f, 0.55f }, GL_LINEAR, GL_CLAMP_TO_EDGE, 0, 0, "left out" },
	{ "one-side-out", { 0.75f, 0.625f, 1.0f / 32, 1.0f / 32 }, { 0, 0 }, { 0.1f, 0.1f },
	  { 0.9f, 0.55f }, GL_LINEAR, GL_CLAMP_TO_EDGE, 0, 0, "kept" },
	{ "offset", { 0.75f, 0.625f, 1.0f / 32, 1.0f / 32 }, { 0.125f, 0 }, { 0.1f, 0.1f },
	  { 0.6f, 0.55f }, GL_NEAREST, GL_CLAMP_TO_EDGE, 0, 0, "kept" },
	{ "mipmap", { 0.75f, 0.625f, 1.0f / 32, 1.0f / 32 }, { 0, 0 }, { 0.1f, 0.1f },
	  { 0.6f, 0.55f }, GL_NEAREST, GL_CLAMP_TO_EDGE, 1, 0, "kept" },
	{ "scaled", { 0.75f, 0.625f, 1.0f / 32, 1.0f / 32 }, { 0, 0 }, { 0.1f, 0.1f },
	  { 0.6f, 0.55f }, GL_NEAREST, GL_CLAMP_TO_EDGE, 0, 1, "kept" },
	/* (PPSSPP's: a 32 x 32 texture of the PSP's in a 24 x 20 one) */
	{ "npot-edge", { 32.0f / 24, 32.0f / 20, 0.5f / 24, 0.5f / 20 }, { 0, 0 },
	  { -0.25f, -0.25f }, { 1.25f, 1.25f }, GL_LINEAR, GL_CLAMP_TO_EDGE, 0, 0, "left out", 1 },
	{ "npot-edge-near", { 32.0f / 24, 32.0f / 20, 0.5f / 24, 0.5f / 20 }, { 0, 0 },
	  { -0.25f, -0.25f }, { 1.25f, 1.25f }, GL_NEAREST, GL_CLAMP_TO_EDGE, 0, 0, "left out", 1 },
	{ "npot-rt-edge", { 32.0f / 24, 32.0f / 20, 0.5f / 24, 0.5f / 20 }, { 0, 0 },
	  { -0.25f, -0.25f }, { 1.25f, 1.25f }, GL_LINEAR, GL_CLAMP_TO_EDGE, 0, 0, "left out", 2 },
	{ "npot-rt-near", { 32.0f / 24, 32.0f / 20, 0.5f / 24, 0.5f / 20 }, { 0, 0 },
	  { -0.25f, -0.25f }, { 1.25f, 1.25f }, GL_NEAREST, GL_CLAMP_TO_EDGE, 0, 0, "left out", 2 },
	{ "npot-rt-inside", { 0.75f, 0.625f, 0.5f / 24, 0.5f / 20 }, { 0, 0 }, { 0.1f, 0.1f },
	  { 0.6f, 0.55f }, GL_LINEAR, GL_CLAMP_TO_EDGE, 0, 0, "left out", 2 },
	{ "npot-rt-out", { 0.75f, 0.625f, 0.5f / 24, 0.5f / 20 }, { 0, 0 }, { 0.1f, 0.1f },
	  { 0.9f, 0.55f }, GL_LINEAR, GL_CLAMP_TO_EDGE, 0, 0, "kept", 2 },
};

static const char *vs_copy =
	"attribute vec2 p;\n"
	"attribute vec3 uv;\n"
	"varying vec4 v_color0;\n"
	"varying vec2 v_texcoord;\n"
	"void main() {\n"
	"  vec2 st = p * 0.5 + 0.5;\n"
	"  gl_Position = vec4(p, 0.0, 1.0);\n"
	"  v_color0 = vec4(1.0, 1.0, 1.0, 1.0);\n"
	"  v_texcoord = uv.xy;\n"
	"}\n";

static const char *vs_scaled =
	"attribute vec2 p;\n"
	"attribute vec3 uv;\n"
	"uniform float u_scale;\n"
	"varying vec4 v_color0;\n"
	"varying vec2 v_texcoord;\n"
	"void main() {\n"
	"  gl_Position = vec4(p, 0.0, 1.0);\n"
	"  v_color0 = vec4(1.0, 1.0, 1.0, 1.0);\n"
	"  v_texcoord = uv.xy * u_scale;\n"
	"}\n";

/* PPSSPP's TClampST program (God of War's GLSL21) */
static const char *fs_src =
	"precision highp float;\n"
	"uniform sampler2D tex;\n"
	"uniform vec4 u_texclamp;\n"
	"uniform vec2 u_texclampoff;\n"
	"varying vec4 v_color0;\n"
	"varying vec2 v_texcoord;\n"
	"void main() {\n"
	"  vec2 fixedcoord = vec2(clamp(v_texcoord.x, u_texclamp.z, u_texclamp.x - u_texclamp.z) + "
	"u_texclampoff.x, clamp(v_texcoord.y, u_texclamp.w, u_texclamp.y - u_texclamp.w) + "
	"u_texclampoff.y);\n"
	"  gl_FragColor = v_color0 * texture2D(tex, fixedcoord);\n"
	"}\n";

static int wrap_index(int i, int n, GLenum wrap)
{
	if (wrap == GL_REPEAT)
		return ((i % n) + n) % n;
	return i < 0 ? 0 : i >= n ? n - 1 : i;
}

static float texel(int x, int y, int c, GLenum wrap)
{
	return texels[4 * (wrap_index(y, th, wrap) * tw + wrap_index(x, tw, wrap)) + c] / 255.0f;
}

/* the texel(s) at (u, v): false where nearest's coordinate is on a
 * texel's edge (either texel would do) */
static int sample(const struct test *T, float u, float v, float o[4])
{
	if (T->filter == GL_NEAREST) {
		float x = u * tw, y = v * th;

		if (fabsf(x - roundf(x)) < 1e-3f || fabsf(y - roundf(y)) < 1e-3f)
			return 0;
		for (int c = 0; c < 4; c++)
			o[c] = texel((int)floorf(x), (int)floorf(y), c, T->wrap);
		return 1;
	}
	{
		float x = u * tw - 0.5f, y = v * th - 0.5f;
		int i = (int)floorf(x), j = (int)floorf(y);
		float a = x - i, b = y - j;

		for (int c = 0; c < 4; c++)
			o[c] = (1 - a) * (1 - b) * texel(i, j, c, T->wrap) +
			       a * (1 - b) * texel(i + 1, j, c, T->wrap) +
			       (1 - a) * b * texel(i, j + 1, c, T->wrap) + a * b * texel(i + 1, j + 1, c, T->wrap);
		return 1;
	}
}

static GLuint build(const struct test *T)
{
	const char *src[2] = { T->scaled ? vs_scaled : vs_copy, fs_src };
	GLenum type[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	GLuint p = glCreateProgram();
	GLint ok;

	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(type[i]);

		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) {
			char log[1024];

			glGetShaderInfoLog(s, sizeof(log), NULL, log);
			fprintf(stderr, "gltexclamp: %s\n%s\n", log, src[i]);
			exit(2);
		}
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "p");
	glBindAttribLocation(p, 1, "uv");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok)
		return fprintf(stderr, "gltexclamp: a program did not link\n"), exit(2), 0;
	glUseProgram(p);
	glUniform1i(glGetUniformLocation(p, "tex"), 0);
	glUniform1f(glGetUniformLocation(p, "u_scale"), 1.0f);
	glUniform4fv(glGetUniformLocation(p, "u_texclamp"), 1, T->clamp);
	glUniform2fv(glGetUniformLocation(p, "u_texclampoff"), 1, T->off);
	return p;
}

/* (npot 2) the texture's texels drawn into it by the GPU: the pattern's
 * copy at a texel a pixel, nearest */
static GLuint copy_program(void)
{
	static const char *vs = "attribute vec2 p; varying vec2 c;\n"
	                        "void main() { c = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n";
	static const char *fs = "precision highp float; uniform sampler2D a; varying vec2 c;\n"
	                        "void main() { gl_FragColor = texture2D(a, c); }\n";
	const char *src[2] = { vs, fs };
	GLenum type[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	GLuint p = glCreateProgram();

	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(type[i]);

		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "p");
	glLinkProgram(p);
	return p;
}

int main(int argc, char **argv)
{
	static const GLushort idx[] = { 0, 1, 2, 0, 2, 3 };
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	static uint8_t px[W * H * 4], ref[W * H * 4];
	int run = 0, failed = 0;
	EGLDisplay dpy;
	EGLContext ctx;
	GLuint rt, fbo, tex, vbo, ibo, copy;

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "gltexclamp: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "gltexclamp: no context\n"), 2;

	copy = copy_program();
	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glViewport(0, 0, W, H);
	glGenBuffers(1, &vbo);
	glGenBuffers(1, &ibo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(idx), idx, GL_STATIC_DRAW);

	for (unsigned k = 0; k < sizeof(tests) / sizeof(tests[0]); k++) {
		const struct test *T = &tests[k];
		/* x, y, u, v, 0 at each corner: a vertex buffer, as PPSSPP's */
		const float *a = T->uv0, *b = T->uv1;
		const float verts[] = { -1, -1, a[0], a[1], 0, 1, -1, b[0], a[1], 0,
		                        1, 1, b[0], b[1], 0, -1, 1, a[0], b[1], 0 };
		int worst = 0, wx = 0, wy = 0, wc = 0, got = 0, want = 0, tol, gpu_worst = 0;
		GLuint kept, prog;

		if (argc > 1 && strncmp(T->name, argv[1], strlen(argv[1])))
			continue;
		run++;
		tw = T->npot ? 24 : 16;
		th = T->npot ? 20 : 16;
		for (int i = 0; i < tw * th; i++) {
			texels[4 * i] = (i * 37) & 255;
			texels[4 * i + 1] = (i * 11 + 40) & 255;
			texels[4 * i + 2] = 255 - ((i * 23) & 255);
			texels[4 * i + 3] = 96 + (i * 7) % 160;
		}
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STREAM_DRAW);
		glEnableVertexAttribArray(0);
		glEnableVertexAttribArray(1);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
		glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
		                      (void *)(2 * sizeof(float)));
		glGenTextures(1, &tex);
		glBindTexture(GL_TEXTURE_2D, tex);
		if (T->npot == 2) {
			GLuint src, fbo2;

			glGenTextures(1, &src);
			glBindTexture(GL_TEXTURE_2D, src);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glBindTexture(GL_TEXTURE_2D, tex);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			glGenFramebuffers(1, &fbo2);
			glBindFramebuffer(GL_FRAMEBUFFER, fbo2);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
			glViewport(0, 0, tw, th);
			glBindTexture(GL_TEXTURE_2D, src);
			glUseProgram(copy);
			glUniform1i(glGetUniformLocation(copy, "a"), 0);
			glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
			glBindFramebuffer(GL_FRAMEBUFFER, fbo);
			glViewport(0, 0, W, H);
			glDeleteFramebuffers(1, &fbo2);
			glDeleteTextures(1, &src);
			glBindTexture(GL_TEXTURE_2D, tex);
		} else {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
		}
		if (T->mip)
			glGenerateMipmap(GL_TEXTURE_2D);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
		                T->mip ? GL_LINEAR_MIPMAP_NEAREST : T->filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, T->filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, T->wrap);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, T->wrap);
		/* the clamp kept: the driver made the program with SGX_NO_TEXCLAMP_FREE */
		setenv("SGX_NO_TEXCLAMP_FREE", "1", 1);
		kept = build(T);
		glClearColor(0, 0, 0, 0);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, ref);
		unsetenv("SGX_NO_TEXCLAMP_FREE");
		prog = build(T);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		glDeleteProgram(kept);
		glDeleteProgram(prog);
		for (int i = 0; i < W * H * 4; i++)
			gpu_worst = abs(px[i] - ref[i]) > gpu_worst ? abs(px[i] - ref[i]) : gpu_worst;
		tol = T->filter == GL_NEAREST && !T->mip ? 0 : 12;
		for (int y = 0; y < H; y++)
			for (int x = 0; x < W; x++) {
				float u = a[0] + (b[0] - a[0]) * (x + 0.5f) / W;
				float v = a[1] + (b[1] - a[1]) * (y + 0.5f) / H;
				float o[4];

				u = fminf(fmaxf(u, T->clamp[2]), T->clamp[0] - T->clamp[2]) + T->off[0];
				v = fminf(fmaxf(v, T->clamp[3]), T->clamp[1] - T->clamp[3]) + T->off[1];
				if (!sample(T, u, v, o))
					continue;
				for (int c = 0; c < 4; c++) {
					int r = (int)(o[c] * 255 + 0.5f), g = px[4 * (y * W + x) + c];

					if (abs(r - g) > worst)
						worst = abs(r - g), wx = x, wy = y, wc = c, got = g, want = r;
				}
			}
		if (worst > tol || gpu_worst > (T->filter == GL_NEAREST ? 0 : 1)) {
			failed++;
			printf("%-12s WRONG: off by %d at %d,%d channel %d (%d, wanted %d), by %d from the "
			       "clamp kept; the clamp should be %s\n", T->name, worst, wx, wy, wc, got, want,
			       gpu_worst, T->should);
		} else {
			printf("%-12s ok (largest difference %d, %d from the clamp kept); the clamp should "
			       "be %s\n", T->name, worst, gpu_worst, T->should);
		}
		fflush(stdout);
		glDeleteTextures(1, &tex);
	}
	printf("%d of %d cases right\n", run - failed, run);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return failed != 0;
}
