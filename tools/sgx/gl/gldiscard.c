/* gldiscard -- discard (docs/research/p105-mesa.md, M19): a quad over the
 * whole target whose varying v goes 0..1 left to right, red where the
 * fragment shader keeps the pixel, over a blue clear; the target read back
 * at four places across (an eighth, three eighths, five, seven).
 *
 *   half      if (v > 0.5) discard
 *   two       if (v < 0.25) discard; if (v > 0.75) discard
 *   uniform   if (u > 0.5) discard, u 1 then 0: nothing, then everything
 *   alpha     a texture's alpha below 0.5 discarded (alpha 0 on the right)
 *   blend     half-transparent red blended, the right half discarded
 *   depth     the right half discarded at depth 0.25, then a green quad at
 *             0.5 with LESS: the discarded pixels must not have written
 *             depth (green on the right, red on the left).  Known wrong
 *             for now: the driver discards by keeping the tile's colour,
 *             and the depth is written all the same (M19); not counted.
 *
 *   sgx-gl gldiscard [--ppm DIR]
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
static GLuint prog_for[8];

static GLuint program(const char *fs)
{
	static const char *vs = "attribute vec4 p; varying float v;\n"
	                        "void main() { v = p.x * 0.5 + 0.5; gl_Position = p; }\n";
	GLuint p = glCreateProgram();
	GLint ok;

	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);

		glShaderSource(s, 1, i ? &fs : &vs, NULL);
		glCompileShader(s);
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "p");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok)
		fprintf(stderr, "gldiscard: a program did not link\n");
	return p;
}

/* the colour at a place across, as a letter: r g b, p (purple: red over
 * blue, half), k (black), ? */
static char at(int x)
{
	const uint8_t *q = px + ((H / 2) * W + x) * 4;
	int r = q[0], g = q[1], b = q[2];

	if (r > 200 && g < 50 && b < 50)
		return 'r';
	if (r < 50 && g > 200 && b < 50)
		return 'g';
	if (r < 50 && g < 50 && b > 200)
		return 'b';
	if (r > 100 && r < 160 && g < 50 && b > 100 && b < 160)
		return 'p';
	if (r < 50 && g < 50 && b < 50)
		return 'k';
	return '?';
}

static void ppm(const char *dir, const char *name)
{
	char path[512];
	FILE *f;

	if (!dir)
		return;
	snprintf(path, sizeof(path), "%s/gldiscard_%s.ppm", dir, name);
	if (!(f = fopen(path, "wb")))
		return;
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (int y = H - 1; y >= 0; y--)
		for (int x = 0; x < W; x++)
			fwrite(px + (y * W + x) * 4, 1, 3, f);
	fclose(f);
}

static void quad(float z)
{
	const float v[] = { -1, -1, z, 1,  1, -1, z, 1,  1, 1, z, 1,
	                    -1, -1, z, 1,  1, 1, z, 1,  -1, 1, z, 1 };

	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, v);
	glDrawArrays(GL_TRIANGLES, 0, 6);
}

int main(int argc, char **argv)
{
	static const char *fs[] = {
		/* 0 half */
		"precision mediump float; varying float v;\n"
		"void main() { if (v > 0.5) discard; gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n",
		/* 1 two */
		"precision mediump float; varying float v;\n"
		"void main() { if (v < 0.25) discard; if (v > 0.75) discard;\n"
		"  gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n",
		/* 2 uniform */
		"precision mediump float; uniform float u;\n"
		"void main() { if (u > 0.5) discard; gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }\n",
		/* 3 alpha */
		"precision mediump float; varying float v; uniform sampler2D t;\n"
		"void main() { vec4 c = texture2D(t, vec2(v, 0.5)); if (c.a < 0.5) discard;\n"
		"  gl_FragColor = vec4(c.rgb, 1.0); }\n",
		/* 4 blend */
		"precision mediump float; varying float v;\n"
		"void main() { if (v > 0.5) discard; gl_FragColor = vec4(1.0, 0.0, 0.0, 0.5); }\n",
		/* 5 green, no discard */
		"precision mediump float;\n"
		"void main() { gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0); }\n",
	};
	static const uint8_t tex[4 * 4] = { 255, 0, 0, 255,  255, 0, 0, 255,
	                                    255, 0, 0, 0,  255, 0, 0, 0 };
	static const struct {
		const char *name, *want;
		int known;      /* known wrong: not counted */
	} cases[] = {
		{ "half", "rrbb" }, { "two", "brrb" }, { "uniform1", "bbbb" }, { "uniform0", "rrrr" },
		{ "alpha", "rrbb" }, { "blend", "ppbb" }, { "depth", "rrgg", 1 },
	};
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	const char *dir = argc > 2 && !strcmp(argv[1], "--ppm") ? argv[2] : NULL;
	int failed = 0, n = 0;
	GLuint rt, zb, fbo, t;
	EGLDisplay dpy;
	EGLContext ctx;

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "gldiscard: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "gldiscard: no GLES 2 context\n"), 2;
	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenRenderbuffers(1, &zb);
	glBindRenderbuffer(GL_RENDERBUFFER, zb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, W, H);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, zb);
	glViewport(0, 0, W, H);
	for (unsigned i = 0; i < sizeof(fs) / sizeof(fs[0]); i++)
		prog_for[i] = program(fs[i]);
	glGenTextures(1, &t);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glEnableVertexAttribArray(0);

	for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
		const char *verdict;
		char got[5];

		glClearColor(0, 0, 1, 1);
		glClearDepthf(1);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		switch (k) {
		case 0: case 1:
			glUseProgram(prog_for[k]);
			quad(0);
			break;
		case 2: case 3:
			glUseProgram(prog_for[2]);
			glUniform1f(glGetUniformLocation(prog_for[2], "u"), k == 2 ? 1.0f : 0.0f);
			quad(0);
			break;
		case 4:
			glUseProgram(prog_for[3]);
			glUniform1i(glGetUniformLocation(prog_for[3], "t"), 0);
			quad(0);
			break;
		case 5:
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			glUseProgram(prog_for[4]);
			quad(0);
			glDisable(GL_BLEND);
			break;
		case 6:
			glEnable(GL_DEPTH_TEST);
			glDepthFunc(GL_LESS);
			glUseProgram(prog_for[0]);
			quad(-0.5f);
			glUseProgram(prog_for[5]);
			quad(0);
			glDisable(GL_DEPTH_TEST);
			break;
		}
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		for (int i = 0; i < 4; i++)
			got[i] = at((2 * i + 1) * W / 8);
		got[4] = 0;
		verdict = !strcmp(got, cases[k].want) ? "ok   " : cases[k].known ? "known" : "WRONG";
		if (!cases[k].known) {
			failed += strcmp(got, cases[k].want) != 0;
			n++;
		}
		printf("%-9s %s: %s (want %s)\n", cases[k].name, verdict, got, cases[k].want);
		ppm(dir, cases[k].name);
	}
	printf("%d of %d cases right\n", n - failed, n);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return failed ? 1 : 0;
}
