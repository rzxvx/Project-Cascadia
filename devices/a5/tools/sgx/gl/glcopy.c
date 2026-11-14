/* glcopy -- copies and blits between textures (docs/research/p105-mesa.md,
 * M46): glCopyImageSubData (ARB_ or NV_copy_image: PPSSPP's framebuffer
 * copies), glBlitFramebuffer (the same size, scaled, flipped) and
 * glCopyTexSubImage2D, each checked against the source as read back --
 * every pixel; a copy exactly, a scaled linear blit to within 2.  Then the
 * copy sampled by a draw, and a copy, a draw into the source and a copy
 * again (their order); and (M48) a draw into a target's right half that
 * samples its left half, the target itself bound as the texture (PPSSPP's
 * bloom without its copies).  Desktop OpenGL through EGL, as PPSSPP has it.
 *
 *   sgx-gl glcopy [CASE]        (CASE: a name's start; all by default)
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
#define H 48

static int cases, failed;
static const char *only;
static PFNGLCOPYIMAGESUBDATAPROC copy_image;
static PFNGLBLITFRAMEBUFFERPROC blit_fb;
static GLuint prog, fbo_of[8];

static const char *vs =
	"#version 110\n"
	"attribute vec4 p;\n"
	"varying vec2 st;\n"
	"void main() { st = p.xy * 0.5 + 0.5; gl_Position = p; }\n";
/* a gradient, a different one for each k */
static const char *fs_grad =
	"#version 110\n"
	"uniform float k;\n"
	"varying vec2 st;\n"
	"void main() { gl_FragColor = vec4(st.x, st.y, fract(st.x * 3.0 + k), 0.25 + 0.5 * k); }\n";
static const char *fs_tex =
	"#version 110\n"
	"uniform sampler2D t;\n"
	"varying vec2 st;\n"
	"void main() { gl_FragColor = texture2D(t, st); }\n";
/* (M48) the left half of t over the whole quad */
static const char *fs_left =
	"#version 110\n"
	"uniform sampler2D t;\n"
	"varying vec2 st;\n"
	"void main() { gl_FragColor = texture2D(t, vec2(st.x * 0.5, st.y)); }\n";

static const float quad[] = { -1, -1, 0, 1, 1, -1, 0, 1, 1, 1, 0, 1,
                              -1, -1, 0, 1, 1, 1, 0, 1, -1, 1, 0, 1 };

static GLuint build(const char *fs)
{
	const char *src[2] = { vs, fs };
	GLenum type[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	GLuint p = glCreateProgram();
	GLint ok;

	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(type[i]);

		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "p");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		fprintf(stderr, "glcopy: a program did not link\n");
		exit(2);
	}
	return p;
}

/* a W x H RGBA8 texture and its framebuffer */
static GLuint texture(int w, int h)
{
	GLuint t, f;

	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glGenFramebuffers(1, &f);
	glBindFramebuffer(GL_FRAMEBUFFER, f);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
	if (t < 8)
		fbo_of[t] = f;
	return t;
}

static void fill(GLuint t, int w, int h, float k)
{
	glBindFramebuffer(GL_FRAMEBUFFER, fbo_of[t]);
	glViewport(0, 0, w, h);
	glUseProgram(prog);
	glUniform1f(glGetUniformLocation(prog, "k"), k);
	glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void clear(GLuint t, float r, float g, float b, float a)
{
	glBindFramebuffer(GL_FRAMEBUFFER, fbo_of[t]);
	glClearColor(r, g, b, a);
	glClear(GL_COLOR_BUFFER_BIT);
}

static void read(GLuint t, int w, int h, uint8_t *px)
{
	glBindFramebuffer(GL_FRAMEBUFFER, fbo_of[t]);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
}

static int wanted(const char *name)
{
	return !only || !strncmp(name, only, strlen(only));
}

/* got against want, pixels [x0, x1) x [y0, y1) of rows of w */
static int compare(const char *name, const uint8_t *got, const uint8_t *want, int w, int x0, int y0,
                   int x1, int y1, int tol)
{
	int worst = 0, wx = 0, wy = 0, wc = 0;

	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
			for (int c = 0; c < 4; c++) {
				int d = abs(got[4 * (y * w + x) + c] - want[4 * (y * w + x) + c]);

				if (d > worst)
					worst = d, wx = x, wy = y, wc = c;
			}
	cases++;
	if (worst > tol) {
		failed++;
		printf("%-16s WRONG: off by %d at %d,%d channel %d (%d, wanted %d)\n", name, worst, wx,
		       wy, wc, got[4 * (wy * w + wx) + wc], want[4 * (wy * w + wx) + wc]);
		return 0;
	}
	printf("%-16s ok (largest difference %d)\n", name, worst);
	return 1;
}

int main(int argc, char **argv)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_MAJOR_VERSION, 2, EGL_CONTEXT_MINOR_VERSION, 1, EGL_NONE };
	static uint8_t a[W * H * 4], b[W * H * 4], want[W * H * 4];
	EGLDisplay dpy;
	EGLContext ctx;
	GLuint A, B, C, D, S, ptex;
	const char *ext;

	only = argc > 1 ? argv[1] : NULL;
	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_API))
		return fprintf(stderr, "glcopy: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glcopy: no OpenGL context\n"), 2;
	ext = (const char *)glGetString(GL_EXTENSIONS);
	if (ext && strstr(ext, "GL_ARB_copy_image"))
		copy_image = (PFNGLCOPYIMAGESUBDATAPROC)eglGetProcAddress("glCopyImageSubData");
	else if (ext && strstr(ext, "GL_NV_copy_image"))
		copy_image = (PFNGLCOPYIMAGESUBDATAPROC)eglGetProcAddress("glCopyImageSubDataNV");
	blit_fb = (PFNGLBLITFRAMEBUFFERPROC)eglGetProcAddress("glBlitFramebuffer");

	prog = build(fs_grad);
	ptex = build(fs_tex);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, quad);
	A = texture(W, H);
	B = texture(W, H);
	C = texture(W, H);
	D = texture(W, H);
	S = texture(W, H);
	if (A >= 8 || S >= 8)
		return fprintf(stderr, "glcopy: texture names past 7\n"), 2;

	fill(A, W, H, 0.3f);
	read(A, W, H, a);

	if (copy_image && wanted("copy-image")) {
		clear(B, 0, 0, 0, 0);
		copy_image(A, GL_TEXTURE_2D, 0, 0, 0, 0, B, GL_TEXTURE_2D, 0, 0, 0, 0, W, H, 1);
		read(B, W, H, b);
		compare("copy-image", b, a, W, 0, 0, W, H, 0);
	} else if (!copy_image) {
		printf("copy-image       (no ARB_ or NV_copy_image)\n");
	}
	if (copy_image && wanted("copy-image-sub")) {
		/* a 20 x 12 piece from (5, 7) to (30, 30); the rest kept */
		clear(B, 0.5f, 0.25f, 0.75f, 1);
		read(B, W, H, want);
		for (int y = 0; y < 12; y++)
			memcpy(want + 4 * ((30 + y) * W + 30), a + 4 * ((7 + y) * W + 5), 20 * 4);
		copy_image(A, GL_TEXTURE_2D, 0, 5, 7, 0, B, GL_TEXTURE_2D, 0, 30, 30, 0, 20, 12, 1);
		read(B, W, H, b);
		compare("copy-image-sub", b, want, W, 0, 0, W, H, 0);
	}
	if (blit_fb && wanted("blit")) {
		/* the same size, nearest */
		clear(C, 0, 0, 0, 0);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_of[A]);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_of[C]);
		blit_fb(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		read(C, W, H, b);
		compare("blit", b, a, W, 0, 0, W, H, 0);
	}
	if (blit_fb && wanted("blit-flip")) {
		/* upside down: row y from row H - 1 - y */
		for (int y = 0; y < H; y++)
			memcpy(want + 4 * y * W, a + 4 * (H - 1 - y) * W, 4 * W);
		clear(C, 0, 0, 0, 0);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_of[A]);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_of[C]);
		blit_fb(0, 0, W, H, 0, H, W, 0, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		read(C, W, H, b);
		compare("blit-flip", b, want, W, 0, 0, W, H, 0);
	}
	if (blit_fb && wanted("blit-half")) {
		/* to half the size, linear: each pixel the four's average (the
		 * centres fall between them); the rest kept */
		clear(C, 0, 0, 0, 0);
		read(C, W, H, want);
		for (int y = 0; y < H / 2; y++)
			for (int x = 0; x < W / 2; x++)
				for (int c = 0; c < 4; c++)
					want[4 * (y * W + x) + c] =
						(a[4 * (2 * y * W + 2 * x) + c] + a[4 * (2 * y * W + 2 * x + 1) + c] +
						 a[4 * ((2 * y + 1) * W + 2 * x) + c] +
						 a[4 * ((2 * y + 1) * W + 2 * x + 1) + c] + 2) / 4;
		glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_of[A]);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_of[C]);
		blit_fb(0, 0, W, H, 0, 0, W / 2, H / 2, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		read(C, W, H, b);
		compare("blit-half", b, want, W, 0, 0, W, H, 2);
	}
	if (wanted("copytex")) {
		/* glCopyTexSubImage2D: a 24 x 16 piece of A's framebuffer from
		 * (8, 4) into D at (2, 3) */
		clear(D, 1, 0, 0, 1);
		read(D, W, H, want);
		for (int y = 0; y < 16; y++)
			memcpy(want + 4 * ((3 + y) * W + 2), a + 4 * ((4 + y) * W + 8), 24 * 4);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo_of[A]);
		glBindTexture(GL_TEXTURE_2D, D);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 2, 3, 8, 4, 24, 16);
		read(D, W, H, b);
		compare("copytex", b, want, W, 0, 0, W, H, 0);
	}
	if (copy_image && wanted("copy-sampled")) {
		/* B a copy of A, drawn as a texture into S */
		clear(B, 0, 0, 0, 0);
		copy_image(A, GL_TEXTURE_2D, 0, 0, 0, 0, B, GL_TEXTURE_2D, 0, 0, 0, 0, W, H, 1);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo_of[S]);
		glViewport(0, 0, W, H);
		glUseProgram(ptex);
		glBindTexture(GL_TEXTURE_2D, B);
		glUniform1i(glGetUniformLocation(ptex, "t"), 0);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		read(S, W, H, b);
		compare("copy-sampled", b, a, W, 0, 0, W, H, 0);
	}
	if (copy_image && wanted("copy-order")) {
		/* copy, draw another gradient into A, copy into C: B the first, C
		 * the second */
		static uint8_t a2[W * H * 4], c[W * H * 4];

		fill(A, W, H, 0.3f);
		copy_image(A, GL_TEXTURE_2D, 0, 0, 0, 0, B, GL_TEXTURE_2D, 0, 0, 0, 0, W, H, 1);
		fill(A, W, H, 0.7f);
		copy_image(A, GL_TEXTURE_2D, 0, 0, 0, 0, C, GL_TEXTURE_2D, 0, 0, 0, 0, W, H, 1);
		read(A, W, H, a2);
		read(B, W, H, b);
		read(C, W, H, c);
		if (compare("copy-order 1st", b, a, W, 0, 0, W, H, 0))
			compare("copy-order 2nd", c, a2, W, 0, 0, W, H, 0);
	}
	if (wanted("self-sample")) {
		/* (M48) S's right half drawn from its left half, S itself the
		 * texture: the left half as it was, the right half a copy of it */
		GLuint pleft = build(fs_left);

		glBindFramebuffer(GL_FRAMEBUFFER, fbo_of[S]);
		glViewport(0, 0, W, H);
		glUseProgram(prog);
		glUniform1f(glGetUniformLocation(prog, "k"), 0.4f);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		read(S, W, H, want);
		for (int y = 0; y < H; y++)
			memcpy(want + 4 * (y * W + W / 2), want + 4 * (y * W), W / 2 * 4);
		glViewport(W / 2, 0, W / 2, H);
		glUseProgram(pleft);
		glBindTexture(GL_TEXTURE_2D, S);
		glUniform1i(glGetUniformLocation(pleft, "t"), 0);
		glDrawArrays(GL_TRIANGLES, 0, 6);
		read(S, W, H, b);
		compare("self-sample", b, want, W, 0, 0, W, H, 0);
	}
	printf("%d of %d cases right\n", cases - failed, cases);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return failed != 0;
}
