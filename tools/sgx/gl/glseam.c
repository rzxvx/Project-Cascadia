/* glseam -- triangles that share an edge cover each pixel once (docs/
 * research/p105-mesa.md, M16): a half-transparent black rectangle, two
 * triangles, blended over white, every pixel read back; a pixel the shared
 * edge covers twice comes out darker (weston's panel showed it as dashes).
 * The rectangle is drawn as a fan of two triangles, then as a strip, then
 * as one of many small quads, then as the fan again with its colour from a
 * texture (a program that samples runs the pixels around the triangles
 * too, for the 2x2 blocks' derivatives: they must not write).
 *
 *   sgx-gl glseam
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define W 768
#define H 1024

int main(void)
{
	static const char *vs = "attribute vec4 p; void main() { gl_Position = p; }\n";
	static const char *fs = "precision mediump float;\n"
	                        "void main() { gl_FragColor = vec4(0.0, 0.0, 0.0, 0.5); }\n";
	static const char *fs_tex = "precision mediump float; uniform sampler2D t;\n"
	                            "void main() { gl_FragColor = texture2D(t, vec2(0.5)); }\n";
	static const uint8_t half[4 * 4 * 4] = {
		0, 0, 0, 128, 0, 0, 0, 128, 0, 0, 0, 128, 0, 0, 0, 128,
		0, 0, 0, 128, 0, 0, 0, 128, 0, 0, 0, 128, 0, 0, 0, 128,
		0, 0, 0, 128, 0, 0, 0, 128, 0, 0, 0, 128, 0, 0, 0, 128,
		0, 0, 0, 128, 0, 0, 0, 128, 0, 0, 0, 128, 0, 0, 0, 128,
	};
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	uint8_t *px = malloc(W * H * 4);
	/* the rectangle: x 0..W, y 0..32 pixels, as clip coordinates */
	float y0 = -1, y1 = -1 + 64.0f / H;
	float fan[] = { -1, y0, 1, y0, 1, y1, -1, y1 };
	float strip[] = { -1, y0, 1, y0, -1, y1, 1, y1 };
	float grid[16 * 6 * 2];
	int failed = 0;
	GLuint rt, fbo, p, pt, tex;
	EGLDisplay dpy;
	EGLContext ctx;

	/* 16 quads of 48 x 32 pixels, each two triangles */
	for (int q = 0; q < 16; q++) {
		float a = -1 + q * 2.0f / 16, b = -1 + (q + 1) * 2.0f / 16;
		float v[12] = { a, y0, b, y0, b, y1, a, y0, b, y1, a, y1 };

		for (int i = 0; i < 12; i++)
			grid[12 * q + i] = v[i];
	}

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glseam: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glseam: no GLES 2 context\n"), 2;
	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glViewport(0, 0, W, H);
	for (int k = 0; k < 2; k++) {
		GLuint prog = glCreateProgram();

		for (int i = 0; i < 2; i++) {
			GLuint s = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);

			glShaderSource(s, 1, i ? (k ? &fs_tex : &fs) : &vs, NULL);
			glCompileShader(s);
			glAttachShader(prog, s);
		}
		glBindAttribLocation(prog, 0, "p");
		glLinkProgram(prog);
		*(k ? &pt : &p) = prog;
	}
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, half);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glEnableVertexAttribArray(0);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	for (int k = 0; k < 4; k++) {
		static const char *name[4] = { "fan", "strip", "16 quads", "textured" };
		int twice = 0, none = 0, fx = -1, fy = -1;

		glUseProgram(k == 3 ? pt : p);
		glClearColor(1, 1, 1, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0,
				      k == 1 ? strip : k == 2 ? grid : fan);
		glDrawArrays(k == 1 ? GL_TRIANGLE_STRIP : k == 2 ? GL_TRIANGLES : GL_TRIANGLE_FAN, 0,
			     k == 2 ? 16 * 6 : 4);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
		for (int y = 0; y < 32; y++)
			for (int x = 0; x < W; x++) {
				int g = px[(y * W + x) * 4 + 1];

				/* once: 0x80; twice: 0x40; not at all: 0xff */
				if (g < 0x60 && !twice++)
					fx = x, fy = y;
				none += g > 0xc0;
			}
		failed += twice || none;
		printf("%-9s %s: %d pixels covered twice (the first at %d,%d), %d not at all\n",
		       name[k], twice || none ? "WRONG" : "ok   ", twice, fx, fy, none);
	}
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	free(px);
	return failed ? 1 : 0;
}
