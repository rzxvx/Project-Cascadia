/* glspeed -- how fast draws go (docs/research/p105-mesa.md, M14): frames of
 * N small quads, each a draw of its own with its own uniform colour, a
 * glFinish a frame; the time a frame and draws a second.
 *
 *   sgx-gl glspeed [DRAWS [FRAMES]]      (defaults 100, 20)
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define W 768
#define H 1024

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	int draws = argc > 1 ? atoi(argv[1]) : 100, frames = argc > 2 ? atoi(argv[2]) : 20;
	static const char *vs = "attribute vec4 p; uniform vec2 o;\n"
	                        "void main() { gl_Position = vec4(p.xy * 0.05 + o, 0.0, 1.0); }\n";
	static const char *fs = "precision mediump float; uniform vec4 c;\n"
	                        "void main() { gl_FragColor = c; }\n";
	static const float quad[] = { -1, -1, 0, 1,  1, -1, 0, 1,  1, 1, 0, 1,
	                              -1, -1, 0, 1,  1, 1, 0, 1,  -1, 1, 0, 1 };
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	GLuint rt, fbo, p;
	GLint lo, lc;
	EGLDisplay dpy;
	EGLContext ctx;
	double t0 = 0, t;

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glspeed: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glspeed: no GLES 2 context\n"), 2;
	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
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
	lo = glGetUniformLocation(p, "o");
	lc = glGetUniformLocation(p, "c");
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, quad);

	for (int f = -1; f < frames; f++) {
		if (f == 0)
			t0 = now();     /* the first frame warms up: not counted */
		glClearColor(0, 0, 0.2f, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		for (int i = 0; i < draws; i++) {
			glUniform2f(lo, -0.9f + 1.8f * (i % 20) / 19, -0.9f + 1.8f * (i / 20 % 20) / 19);
			glUniform4f(lc, (i % 7) / 6.0f, (i % 5) / 4.0f, (i % 3) / 2.0f, 1);
			glDrawArrays(GL_TRIANGLES, 0, 6);
		}
		glFinish();
	}
	t = now() - t0;
	printf("%d frames of %d draws: %.2f ms a frame, %.0f draws a second\n", frames, draws,
	       t * 1000 / frames, frames * draws / t);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return 0;
}
