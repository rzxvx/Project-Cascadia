/* glstencil -- the stencil test (docs/research/p105-mesa.md, M23): a target
 * with a packed depth/stencil buffer; quads drawn with stencil ops, then a
 * green quad over the whole target with a stencil test.  Four places across
 * are read: g green (the test passed), k black (failed).  All of a case in
 * one render: the stencil lives in the tiles.
 *
 *   sgx-gl glstencil [CASE...]
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 256
#define H 256

static uint8_t px[W * H * 4];

/* a quad over x0..x1 (clip space), all of y, at depth z */
static void quad(float x0, float x1, float z)
{
	const float v[] = { x0, -1, z, 1,  x1, -1, z, 1,  x1, 1, z, 1,
	                    x0, -1, z, 1,  x1, 1, z, 1,  x0, 1, z, 1 };

	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, v);
	glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void stencil(GLenum func, int ref, unsigned mask, GLenum fail, GLenum zfail, GLenum zpass)
{
	glStencilFunc(func, ref, mask);
	glStencilOp(fail, zfail, zpass);
}

int main(int argc, char **argv)
{
	static const char *vs = "attribute vec4 p; void main() { gl_Position = p; }\n";
	static const char *fs = "precision mediump float; uniform vec4 c;\n"
	                        "void main() { gl_FragColor = c; }\n";
	static const char *names[] = {
		"replace_equal", "replace_notequal", "incr_twice", "decr_wrap", "invert",
		"zero", "write_mask", "compare_mask", "depth_fail", "never", "less",
		"clear_value",
	};
	/* what the four places (eighths 1, 3, 5, 7) should be */
	static const char *want[] = {
		"ggkk", "kkgg", "ggkk", "ggkk", "ggkk",
		"kkgg", "ggkk", "gggg", "kkgg", "kkkk", "ggkk",
		"ggkk",
	};
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	int failed = 0, run = 0;
	GLuint rt, zs, fbo, p;
	GLint col;
	EGLDisplay dpy;
	EGLContext ctx;

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glstencil: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glstencil: no GLES 2 context\n"), 2;
	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenRenderbuffers(1, &zs);
	glBindRenderbuffer(GL_RENDERBUFFER, zs);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, W, H);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, zs);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, zs);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		return fprintf(stderr, "glstencil: the framebuffer is not complete\n"), 2;
	glViewport(0, 0, W, H);
	p = glCreateProgram();
	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);

		glShaderSource(s, 1, i ? &fs : &vs, NULL);
		glCompileShader(s);
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "p");
	glLinkProgram(p);
	glUseProgram(p);
	col = glGetUniformLocation(p, "c");
	glEnableVertexAttribArray(0);

	for (unsigned k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
		int wanted = argc < 2;
		char got[5];

		for (int i = 1; i < argc; i++)
			wanted |= !strncmp(names[k], argv[i], strlen(argv[i]));
		if (!wanted)
			continue;
		run++;
		glDisable(GL_DEPTH_TEST);
		glDisable(GL_STENCIL_TEST);
		glStencilMask(0xff);
		glClearColor(0, 0, 0, 1);
		glClearStencil(k == 11 ? 5 : 0);
		glClearDepthf(1);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		glEnable(GL_STENCIL_TEST);
		/* the marks: black quads, colour unchanged where they draw */
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
		glUniform4f(col, 1, 0, 0, 1);
		switch (k) {
		case 0: case 1:         /* the left half 1 */
			stencil(GL_ALWAYS, 1, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
			quad(-1, 0, 0);
			break;
		case 2:                 /* the left half 2 */
			stencil(GL_ALWAYS, 0, 0xff, GL_KEEP, GL_KEEP, GL_INCR);
			quad(-1, 0, 0);
			quad(-1, 0, 0);
			break;
		case 3:                 /* 0 - 1 wrapped: 255 on the left half */
			stencil(GL_ALWAYS, 0, 0xff, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
			quad(-1, 0, 0);
			break;
		case 4:                 /* ~0: 255 on the left half */
			stencil(GL_ALWAYS, 0, 0xff, GL_KEEP, GL_KEEP, GL_INVERT);
			quad(-1, 0, 0);
			break;
		case 5:                 /* all 7, then the left half 0 */
			stencil(GL_ALWAYS, 7, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
			quad(-1, 1, 0);
			stencil(GL_ALWAYS, 7, 0xff, GL_KEEP, GL_KEEP, GL_ZERO);
			quad(-1, 0, 0);
			break;
		case 6:                 /* 0xff through write mask 0x0f: 0x0f on the left */
			glStencilMask(0x0f);
			stencil(GL_ALWAYS, 0xff, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
			quad(-1, 0, 0);
			glStencilMask(0xff);
			break;
		case 7:                 /* 0x10 on the left half */
			stencil(GL_ALWAYS, 0x10, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
			quad(-1, 0, 0);
			break;
		case 8:                 /* a near quad on the left, then one behind it all
			                 * across: depth fails on the left -- REPLACE 1 there */
			glEnable(GL_DEPTH_TEST);
			glDepthFunc(GL_LESS);
			stencil(GL_ALWAYS, 0, 0xff, GL_KEEP, GL_KEEP, GL_KEEP);
			quad(-1, 0, -0.5f);
			stencil(GL_ALWAYS, 1, 0xff, GL_KEEP, GL_REPLACE, GL_KEEP);
			quad(-1, 1, 0.5f);
			glDisable(GL_DEPTH_TEST);
			break;
		case 11:                /* cleared to 5, the right half 6 */
			stencil(GL_ALWAYS, 6, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
			quad(0, 1, 0);
			break;
		case 9: case 10:        /* the left half 3 */
			stencil(GL_ALWAYS, 3, 0xff, GL_KEEP, GL_KEEP, GL_REPLACE);
			quad(-1, 0, 0);
			break;
		}
		/* the test: green over everything where it passes */
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glUniform4f(col, 0, 1, 0, 1);
		switch (k) {
		case 0: stencil(GL_EQUAL, 1, 0xff, GL_KEEP, GL_KEEP, GL_KEEP); break;
		case 1: stencil(GL_NOTEQUAL, 1, 0xff, GL_KEEP, GL_KEEP, GL_KEEP); break;
		case 2: stencil(GL_EQUAL, 2, 0xff, GL_KEEP, GL_KEEP, GL_KEEP); break;
		case 3: case 4: stencil(GL_EQUAL, 255, 0xff, GL_KEEP, GL_KEEP, GL_KEEP); break;
		case 5: stencil(GL_EQUAL, 7, 0xff, GL_KEEP, GL_KEEP, GL_KEEP); break;
		case 6: stencil(GL_EQUAL, 0x0f, 0xff, GL_KEEP, GL_KEEP, GL_KEEP); break;
		/* 0x10 or 0 through compare mask 0x0f: both 0 -- equal 0 everywhere */
		case 7: stencil(GL_EQUAL, 0, 0x0f, GL_KEEP, GL_KEEP, GL_KEEP); break;
		/* the right half kept 0 (depth passed there) */
		case 8: stencil(GL_EQUAL, 0, 0xff, GL_KEEP, GL_KEEP, GL_KEEP); break;
		case 9: stencil(GL_NEVER, 3, 0xff, GL_KEEP, GL_KEEP, GL_KEEP); break;
		/* ref 2 < 3 on the left, 2 < 0 not on the right */
		case 10: stencil(GL_LESS, 2, 0xff, GL_KEEP, GL_KEEP, GL_KEEP); break;
		case 11: stencil(GL_EQUAL, 5, 0xff, GL_KEEP, GL_KEEP, GL_KEEP); break;
		}
		quad(-1, 1, 0);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		for (int i = 0; i < 4; i++) {
			const uint8_t *q = px + ((H / 2) * W + (2 * i + 1) * W / 8) * 4;

			got[i] = q[1] > 200 && q[0] < 50 ? 'g' : q[0] < 50 && q[1] < 50 && q[2] < 50 ? 'k' : '?';
		}
		got[4] = 0;
		failed += strcmp(got, want[k]) != 0;
		printf("%-17s %s: %s (want %s)\n", names[k], strcmp(got, want[k]) ? "WRONG" : "ok   ",
		       got, want[k]);
	}
	printf("%d of %d cases right\n", run - failed, run);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return failed ? 1 : 0;
}
