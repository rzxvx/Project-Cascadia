/* glspeed -- how fast draws go (docs/research/p105-mesa.md, M14): frames of
 * N small quads, each a draw of its own with its own uniform colour, a
 * glFinish a frame -- or, with "flush", a glFlush, the CPU going on with the
 * next frame while the GPU draws the last (M27), and two targets in turn;
 * the time a frame and draws a second.  "sprites": SDL's renderer's way
 * instead (M34) -- textured quads blended, one of 8 textures each, their
 * vertices (x y, r g b a, u v floats) made by the CPU every frame into one
 * array the draws point into, each a draw of its own.
 *
 *   sgx-gl glspeed [DRAWS [FRAMES [flush|sprites]]]      (defaults 100, 20)
 *
 * GLSPEED_TEX=n: the sprites take n textures in turn (8); GLSPEED_VBO=1:
 * their vertices in a buffer, glBufferData once a frame (client arrays);
 * "fill": the sprites each the whole target (what the GPU takes to fill:
 * GLSPEED_BLEND=0 without blending) */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
	int flush = argc > 3 && !strcmp(argv[3], "flush");
	int fill = argc > 3 && !strcmp(argv[3], "fill");
	int sprites = argc > 3 && (!strcmp(argv[3], "sprites") || fill);
	static const char *vss = "attribute vec2 p; attribute vec4 c; attribute vec2 uv;\n"
	                         "varying vec4 vc; varying vec2 vuv;\n"
	                         "void main() { gl_Position = vec4(p, 0.0, 1.0); vc = c; vuv = uv; }\n";
	static const char *fss = "precision mediump float; uniform sampler2D t;\n"
	                         "varying vec4 vc; varying vec2 vuv;\n"
	                         "void main() { gl_FragColor = texture2D(t, vuv) * vc; }\n";
	/* GLSPEED_FS=1: the sprites' colour alone, no texture; 2: the texel
	 * alone */
	static const char *fss1 = "precision mediump float; varying vec4 vc; varying vec2 vuv;\n"
	                          "void main() { gl_FragColor = vc + vec4(vuv, 0.0, 0.0) * 0.001; }\n";
	static const char *fss2 = "precision mediump float; uniform sampler2D t; varying vec2 vuv;\n"
	                          "varying vec4 vc;\n"
	                          "void main() { gl_FragColor = texture2D(t, vuv) + vc * 0.001; }\n";
	/* 3: a constant colour; 4: the colour varying as it is */
	static const char *fss3 = "precision mediump float; varying vec4 vc; varying vec2 vuv;\n"
	                          "void main() { gl_FragColor = vec4(0.5, 0.25, 0.75, 1.0); }\n";
	static const char *fss4 = "precision mediump float; varying vec4 vc; varying vec2 vuv;\n"
	                          "void main() { gl_FragColor = vc; }\n";
	int fsv = getenv("GLSPEED_FS") ? atoi(getenv("GLSPEED_FS")) : 0;
	GLuint tex[8], vbo = 0;
	float *verts = NULL;
	int ntex = getenv("GLSPEED_TEX") ? atoi(getenv("GLSPEED_TEX")) : 8;
	int use_vbo = getenv("GLSPEED_VBO") && atoi(getenv("GLSPEED_VBO"));
	static const char *vs = "attribute vec4 p; uniform vec2 o;\n"
	                        "void main() { gl_Position = vec4(p.xy * 0.05 + o, 0.0, 1.0); }\n";
	static const char *fs = "precision mediump float; uniform vec4 c;\n"
	                        "void main() { gl_FragColor = c; }\n";
	static const float quad[] = { -1, -1, 0, 1,  1, -1, 0, 1,  1, 1, 0, 1,
	                              -1, -1, 0, 1,  1, 1, 0, 1,  -1, 1, 0, 1 };
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	GLuint rt[2], fbo[2], p;
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
	glGenTextures(2, rt);
	glGenFramebuffers(2, fbo);
	for (int i = 0; i < 2; i++) {
		glBindTexture(GL_TEXTURE_2D, rt[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt[i], 0);
	}
	glViewport(0, 0, W, H);
	p = glCreateProgram();
	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(i ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);

		glShaderSource(s, 1, i ? (sprites ? (fsv == 1 ? &fss1 : fsv == 2 ? &fss2 : fsv == 3 ? &fss3 :
		                                     fsv == 4 ? &fss4 : &fss) : &fs) :
		                     (sprites ? &vss : &vs), NULL);
		glCompileShader(s);
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "p");
	glBindAttribLocation(p, 1, "c");
	glBindAttribLocation(p, 2, "uv");
	glLinkProgram(p);
	glUseProgram(p);
	lo = glGetUniformLocation(p, "o");
	lc = glGetUniformLocation(p, "c");
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 0, quad);
	if (sprites) {
		uint8_t img[64 * 64 * 4];

		glGenTextures(8, tex);
		for (int k = 0; k < 8; k++) {
			for (int i = 0; i < 64 * 64; i++) {
				img[4 * i] = k * 30;
				img[4 * i + 1] = i & 255;
				img[4 * i + 2] = i >> 4;
				img[4 * i + 3] = 200;
			}
			glBindTexture(GL_TEXTURE_2D, tex[k]);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		}
		glUniform1i(glGetUniformLocation(p, "t"), 0);
		glEnableVertexAttribArray(1);
		glEnableVertexAttribArray(2);
		if (!getenv("GLSPEED_BLEND") || atoi(getenv("GLSPEED_BLEND")))
			glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		verts = malloc(draws * 4 * 8 * sizeof(float));
		if (use_vbo)
			glGenBuffers(1, &vbo);
	}

	for (int f = -1; f < frames; f++) {
		if (f == 0)
			t0 = now();     /* the first frame warms up: not counted */
		glBindFramebuffer(GL_FRAMEBUFFER, fbo[flush && (f & 1)]);
		glClearColor(0, 0, 0.2f, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		for (int i = 0; sprites && i < draws; i++) {
			float x = -0.95f + 1.8f * (i % 20) / 19, y = -0.95f + 1.8f * (i / 20 % 20) / 19;
			float *q = verts + i * 4 * 8;

			for (int k = 0; k < 4; k++) {
				q[8 * k] = fill ? (k & 1) * 2 - 1.0f : x + (k & 1) * 0.1f;
				q[8 * k + 1] = fill ? (k >> 1) * 2 - 1.0f : y + (k >> 1) * 0.1f;
				q[8 * k + 2] = q[8 * k + 3] = q[8 * k + 4] = 1;
				q[8 * k + 5] = 0.9f;
				q[8 * k + 6] = k & 1;
				q[8 * k + 7] = k >> 1;
			}
		}
		if (use_vbo) {
			glBindBuffer(GL_ARRAY_BUFFER, vbo);
			glBufferData(GL_ARRAY_BUFFER, draws * 4 * 8 * sizeof(float), verts, GL_STREAM_DRAW);
		}
		for (int i = 0; i < draws; i++) {
			if (sprites) {
				const float *q = use_vbo ? (const float *)NULL + i * 4 * 8 : verts + i * 4 * 8;

				glBindTexture(GL_TEXTURE_2D, tex[i % (ntex > 0 && ntex <= 8 ? ntex : 8)]);
				glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 32, q);
				glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 32, q + 2);
				glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, q + 6);
				glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
				continue;
			}
			glUniform2f(lo, -0.9f + 1.8f * (i % 20) / 19, -0.9f + 1.8f * (i / 20 % 20) / 19);
			glUniform4f(lc, (i % 7) / 6.0f, (i % 5) / 4.0f, (i % 3) / 2.0f, 1);
			glDrawArrays(GL_TRIANGLES, 0, 6);
		}
		if (flush && f < frames - 1)
			glFlush();
		else
			glFinish();
	}
	t = now() - t0;
	printf("%d frames of %d draws: %.2f ms a frame, %.0f draws a second\n", frames, draws,
	       t * 1000 / frames, frames * draws / t);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return 0;
}
