/* glshader -- one fragment shader's cost (docs/research/p105-mesa.md, M46):
 * VS and FS from files, a quad over the whole target LAYERS times a frame,
 * a glFinish a frame; the time a frame.  With SGX_DEBUG_SHADER=1 the driver
 * prints the program it made (tools/iosgpu/usse-dis.py reads the words), with
 * SGX_DEBUG=sync each render's time on the GPU.
 *
 *   sgx-gl glshader VS FS [-w W] [-h H] [-n LAYERS] [-f FRAMES] [-b|-A] [-M]
 *                   [-T SIZE] [-U NAME=X[,Y,Z,W]]...
 *
 *   -n  layers a frame (10)      -f  frames, after one not counted (20)
 *   -b  blended (source alpha, one less source alpha)
 *   -A  blended additively (one, one: a discard there is a colour of 0)
 *   -M  the target's alpha kept (glColorMask(1, 1, 1, 0))
 *   -T  the texture on unit 0, SIZE x SIZE (64), bilinear
 *   -U  a float or vector uniform set to these values (up to 8 of them)
 *
 * The vertex shader's attributes: `p` (vec4: x, y over the target, z 0.5,
 * w 1), `c` (vec4 colour, a gradient), `uv` (vec4: u, v from 0 to 1, z w
 * 0.5), and a fourth (location 3) the colour too; its uniforms and the fragment shader's are set by type -- samplers
 * to unit 0, floats and vectors to 0.5, matrices to the identity, unless
 * -U names them -- so a shader as an emulator made it can be dropped in
 * with its varyings and uniform names as they are (PPSSPP's u_texclamp at
 * 0.5 clamps every lookup to one texel: -U u_texclamp=1,1,0.001,0.001). */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	long n;
	char *s;

	if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) ||
	    !(s = malloc(n + 1)) || fread(s, 1, n, f) != (size_t)n) {
		fprintf(stderr, "glshader: cannot read %s\n", path);
		exit(2);
	}
	s[n] = 0;
	fclose(f);
	return s;
}

static GLuint shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok;

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[2048];

		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		fprintf(stderr, "glshader: %s\n", log);
		exit(2);
	}
	return s;
}

int main(int argc, char **argv)
{
	int w = 480, h = 272, layers = 10, frames = 20, blend = 0, mask = 0, tsize = 64, opt;
	static const float quad[6][3][4] = {
		{ { -1, -1, .5f, 1 }, { 1, .2f, .1f, 1 }, { 0, 0, .5f, .5f } },
		{ { 1, -1, .5f, 1 }, { .2f, 1, .3f, .8f }, { 1, 0, .5f, .5f } },
		{ { 1, 1, .5f, 1 }, { .3f, .4f, 1, .6f }, { 1, 1, .5f, .5f } },
		{ { -1, -1, .5f, 1 }, { 1, .2f, .1f, 1 }, { 0, 0, .5f, .5f } },
		{ { 1, 1, .5f, 1 }, { .3f, .4f, 1, .6f }, { 1, 1, .5f, .5f } },
		{ { -1, 1, .5f, 1 }, { .9f, .9f, .2f, .9f }, { 0, 1, .5f, .5f } },
	};
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLDisplay dpy;
	EGLContext ctx;
	GLuint rt, fbo, t, p;
	GLint n;
	uint8_t *img;
	double t0 = 0, el;
	struct { char name[64]; float v[4]; } set[8];
	int nset = 0;

	while ((opt = getopt(argc, argv, "w:h:n:f:bAMT:U:")) != -1) {
		switch (opt) {
		case 'w': w = atoi(optarg); break;
		case 'h': h = atoi(optarg); break;
		case 'n': layers = atoi(optarg); break;
		case 'f': frames = atoi(optarg); break;
		case 'b': blend = 1; break;
		case 'A': blend = 2; break;
		case 'M': mask = 1; break;
		case 'T': tsize = atoi(optarg); break;
		case 'U': {
			char *eq = strchr(optarg, '=');

			if (!eq || eq - optarg >= 64 || nset == 8)
				return fprintf(stderr, "glshader: -U NAME=X[,Y,Z,W]\n"), 2;
			memcpy(set[nset].name, optarg, eq - optarg);
			set[nset].name[eq - optarg] = 0;
			memset(set[nset].v, 0, sizeof(set[nset].v));
			sscanf(eq + 1, "%f,%f,%f,%f", &set[nset].v[0], &set[nset].v[1], &set[nset].v[2],
			       &set[nset].v[3]);
			nset++;
			break;
		}
		default: return fprintf(stderr, "glshader: see its source for the options\n"), 2;
		}
	}
	if (argc - optind != 2 || tsize < 1 || tsize > 2048 || layers < 1)
		return fprintf(stderr, "glshader VS FS [-w W] [-h H] [-n LAYERS] [-f FRAMES] [-b|-A] [-M] "
		                       "[-T SIZE]\n"), 2;

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glshader: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glshader: no GLES 2 context\n"), 2;

	img = malloc(tsize * tsize * 4);
	for (int i = 0; i < tsize * tsize * 4; i++)
		img[i] = (i & 3) == 3 ? 128 + (i * 13) % 128 : i * 7;
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tsize, tsize, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	free(img);

	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	glViewport(0, 0, w, h);
	glBindTexture(GL_TEXTURE_2D, t);

	p = glCreateProgram();
	glAttachShader(p, shader(GL_VERTEX_SHADER, slurp(argv[optind])));
	glAttachShader(p, shader(GL_FRAGMENT_SHADER, slurp(argv[optind + 1])));
	glBindAttribLocation(p, 0, "p");
	glBindAttribLocation(p, 1, "c");
	glBindAttribLocation(p, 2, "uv");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &n);
	if (!n) {
		char log[2048];

		glGetProgramInfoLog(p, sizeof(log), NULL, log);
		return fprintf(stderr, "glshader: %s\n", log), 2;
	}
	glUseProgram(p);
	/* every uniform by its type */
	glGetProgramiv(p, GL_ACTIVE_UNIFORMS, &n);
	for (GLint i = 0; i < n; i++) {
		static const float half[16] = { .5f, .5f, .5f, .5f, .5f, .5f, .5f, .5f,
		                                .5f, .5f, .5f, .5f, .5f, .5f, .5f, .5f };
		static const float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
		char name[256];
		GLint size, loc;
		GLenum type;

		glGetActiveUniform(p, i, sizeof(name), NULL, &size, &type, name);
		loc = glGetUniformLocation(p, name);
		switch (type) {
		case GL_SAMPLER_2D: glUniform1i(loc, 0); break;
		case GL_FLOAT: glUniform1fv(loc, size, half); break;
		case GL_FLOAT_VEC2: glUniform2fv(loc, size, half); break;
		case GL_FLOAT_VEC3: glUniform3fv(loc, size, half); break;
		case GL_FLOAT_VEC4: glUniform4fv(loc, size, half); break;
		case GL_FLOAT_MAT4: glUniformMatrix4fv(loc, 1, GL_FALSE, id); break;
		case GL_INT: glUniform1i(loc, 1); break;
		default: break;
		}
		for (int k = 0; k < nset; k++)
			if (!strcmp(name, set[k].name)) {
				if (type == GL_FLOAT) glUniform1fv(loc, 1, set[k].v);
				if (type == GL_FLOAT_VEC2) glUniform2fv(loc, 1, set[k].v);
				if (type == GL_FLOAT_VEC3) glUniform3fv(loc, 1, set[k].v);
				if (type == GL_FLOAT_VEC4) glUniform4fv(loc, 1, set[k].v);
			}
	}
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(quad[0]), quad[0][0]);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(quad[0]), quad[0][1]);
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(quad[0]), quad[0][2]);
	/* (a fourth attribute the linker placed there -- PPSSPP's color0 -- read
	 * from an array too, the colour's, not a constant) */
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(quad[0]), quad[0][1]);
	if (blend) {
		glEnable(GL_BLEND);
		if (blend == 2)
			glBlendFunc(GL_ONE, GL_ONE);
		else
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}

	for (int f = -1; f < frames; f++) {
		if (f == 0)
			t0 = now();
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glClearColor(0, 0, 0.2f, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		if (mask)
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
		for (int l = 0; l < layers; l++)
			glDrawArrays(GL_TRIANGLES, 0, 6);
		glFinish();
	}
	el = now() - t0;
	printf("glshader %s: %d layers of %dx%d%s%s: %.2f ms a frame\n", argv[optind + 1], layers, w, h,
	       blend == 2 ? ", added" : blend ? ", blended" : "", mask ? ", alpha kept" : "", el * 1000 / frames);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return 0;
}
