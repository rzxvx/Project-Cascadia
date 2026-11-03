/* glflat -- which vertex's colour a triangle takes (docs/research/p105-mesa.md,
 * M35): two triangles, GL_TRIANGLES, their vertices red, green and blue (and
 * cyan, magenta, yellow), each triangle's centre read back.  Interpolated,
 * the centres are greys; flat, each is one vertex's colour -- which one is
 * the provoking vertex.  With SGX_STATE=word:mask, finding the ISP's flat
 * shading.
 *
 *   sgx-gl glflat
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define W 64
#define H 64

int
main(void)
{
	static const float pos[] = {
		-1, -1, 0, -1, -1, 0,     /* the first triangle: lower left */
		1, 1, 0, 1, 1, 0,         /* the second: upper right */
	};
	static const float col[] = {
		1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1,
		0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1,
	};
	static const char *names[6] = { "red", "green", "blue", "cyan", "magenta", "yellow" };
	static const char *vs = "attribute vec4 p; attribute vec4 c; varying vec4 v;\n"
	                        "void main() { gl_Position = p; v = c; }\n";
	static const char *fs = "precision mediump float; varying vec4 v;\n"
	                        "void main() { gl_FragColor = v; }\n";
	const char *src[2] = { vs, fs };
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	uint8_t px[4];
	GLuint rt, fbo, p;
	EGLDisplay dpy;
	EGLContext ctx;

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glflat: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glflat: no GLES 2 context\n"), 2;
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

		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "p");
	glBindAttribLocation(p, 1, "c");
	glLinkProgram(p);
	glUseProgram(p);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, pos);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, col);
	glClearColor(0, 0, 0, 0);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	for (int t = 0; t < 2; t++) {
		/* each triangle's centre */
		int x = t ? 5 * W / 6 : W / 6, y = t ? 5 * H / 6 : H / 6;
		const char *name = "interpolated";

		glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
		for (int v = 0; v < 3; v++) {
			const float *c = col + 4 * (3 * t + v);

			if (px[0] == (int)(c[0] * 255) && px[1] == (int)(c[1] * 255) &&
			    px[2] == (int)(c[2] * 255))
				name = names[3 * t + v];
		}
		printf("triangle %d: %3d %3d %3d %3d  %s\n", t, px[0], px[1], px[2], px[3], name);
	}
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return 0;
}
