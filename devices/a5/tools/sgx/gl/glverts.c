/* glverts -- how fast the GPU's vertex and pixel sides go
 * (docs/research/p105-mesa.md, M46): renders of one or more draws of many
 * triangles, each frame a clear, the draws and a glFinish, into a target of
 * the PSP's size; the time a frame and indices a second.  With
 * SGX_DEBUG=sync the driver says each render's own time on the GPU (it waits
 * for each).
 *
 *   sgx-gl glverts [-n INDICES] [-d DRAWS] [-f FRAMES] [-v VARYINGS]
 *                  [-m list|ilist|strip|grid|fill] [-s SIZE] [-w W] [-h H]
 *                  [-c] [-z] [-q] [-b] [-k] [-t] [-T SIZE] [-R] [-N]
 *
 *   -n  indices (or vertices) a draw (3000)       -d  draws a frame (1)
 *   -f  frames measured, after one not (20)       -v  vec4 varyings, 0..7 (2)
 *   -m  list:  glDrawArrays(GL_TRIANGLES), every triangle its own vertices
 *       ilist: the same through indices 0, 1, 2, ... (glDrawElements)
 *       strip: indices as an emulator makes a strip of them into a list
 *              (PPSSPP's IndexGenerator): each vertex in three triangles
 *       grid:  a grid's indices: each vertex in six triangles
 *       fill:  quads over the whole target, n / 6 of them one on another
 *   -s  a triangle's size in pixels (1); 0: all three vertices one point
 *   -w, -h  the target (480 x 272)
 *   -c  the triangles all behind the near plane (clipped away whole)
 *   -z  a depth buffer, tested (LEQUAL) and written
 *   -q  each draw's vertices made by the CPU into the buffer every frame
 *       (glBufferSubData, as an emulator's are)
 *   -b  blended (source alpha, one less source alpha)
 *   -k  alpha tested: pixels with alpha under 0.5 discarded (PPSSPP's way)
 *   -M  the target's alpha kept (glColorMask(1, 1, 1, 0): PPSSPP's way,
 *       the PSP's stencil there)
 *   -t  a texture read by the pixels, SIZE x SIZE (-T, 64), bilinear (-N:
 *       nearest), filled by the CPU -- or, -R, by a render (a target
 *       sampled: the driver reads those linear, not twiddled)
 *
 * The vertices: x y z as floats, colour as 4 bytes, u v as floats -- 24
 * bytes, PPSSPP's decoded layout -- in one buffer object.  The vertex
 * shader transforms by a matrix and writes each varying from the colour and
 * the coordinates; the fragment shader adds them all, so the linker keeps
 * them. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct vtx {
	float x, y, z;
	uint8_t c[4];
	float u, v;
};

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static GLuint shader(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	GLint ok;

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024];

		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		fprintf(stderr, "glverts: %s\n%s\n", log, src);
		exit(2);
	}
	return s;
}

static GLuint program(const char *vs, const char *fs)
{
	GLuint p = glCreateProgram();

	glAttachShader(p, shader(GL_VERTEX_SHADER, vs));
	glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
	glBindAttribLocation(p, 0, "p");
	glBindAttribLocation(p, 1, "c");
	glBindAttribLocation(p, 2, "uv");
	glLinkProgram(p);
	return p;
}

/* a vertex at pixel (px, py) of a w x h target, as clip coordinates the
 * matrix (the identity) leaves alone; its alpha from i */
static void put(struct vtx *v, float px, float py, int w, int h, int behind, unsigned i)
{
	v->x = 2 * px / w - 1;
	v->y = 2 * py / h - 1;
	v->z = behind ? -2.0f : 0.25f;
	v->c[0] = i * 37;
	v->c[1] = i * 11;
	v->c[2] = i * 5;
	v->c[3] = 96 + (i * 53) % 160;
	v->u = px / w;
	v->v = py / h;
}

int main(int argc, char **argv)
{
	int n = 3000, draws = 1, frames = 20, nvary = 2, w = 480, h = 272, behind = 0, depth = 0;
	int tex = 0, cpu = 0, blend = 0, kill = 0, tsize = 64, rtex = 0, nearest = 0, mask = 0, opt;
	float size = 1;
	const char *mode = "list";
	char vs[4096], fs[4096];
	int nv, ni, m = 0;
	struct vtx *verts;
	uint16_t *idx = NULL;
	GLuint vbo, ibo = 0, rt, fbo, zrb, p, t;
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display;
	EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLDisplay dpy;
	EGLContext ctx;
	double t0 = 0, el;

	while ((opt = getopt(argc, argv, "n:d:f:v:m:s:w:h:czqbkMtT:RN")) != -1) {
		switch (opt) {
		case 'n': n = atoi(optarg); break;
		case 'd': draws = atoi(optarg); break;
		case 'f': frames = atoi(optarg); break;
		case 'v': nvary = atoi(optarg); break;
		case 'm': mode = optarg; break;
		case 's': size = atof(optarg); break;
		case 'w': w = atoi(optarg); break;
		case 'h': h = atoi(optarg); break;
		case 'c': behind = 1; break;
		case 'z': depth = 1; break;
		case 'q': cpu = 1; break;
		case 'b': blend = 1; break;
		case 'k': kill = 1; break;
		case 'M': mask = 1; break;
		case 't': tex = 1; break;
		case 'T': tsize = atoi(optarg); break;
		case 'R': rtex = 1; break;
		case 'N': nearest = 1; break;
		default: return fprintf(stderr, "glverts: see its source for the options\n"), 2;
		}
	}
	if (nvary < 0 || nvary > 7 || n < 3 || draws < 1 || tsize < 1 || tsize > 2048 ||
	    size < 0 || w < 1 || h < 1)
		return fprintf(stderr, "glverts: -v 0..7, -n 3 or more, -d 1 or more, -T 1..2048\n"), 2;
	if (tex && nvary < 2)
		nvary = 2;      /* (the texture's coordinates are varying 1) */
	n -= n % 3;
	if (!strcmp(mode, "list")) {
		m = 0;
	} else if (!strcmp(mode, "ilist")) {
		m = 1;
	} else if (!strcmp(mode, "strip")) {
		m = 2;
	} else if (!strcmp(mode, "grid")) {
		m = 3;
	} else if (!strcmp(mode, "fill")) {
		m = 4;
		n -= n % 6;
		if (!n)
			n = 6;
	} else {
		return fprintf(stderr, "glverts: -m list, ilist, strip, grid or fill\n"), 2;
	}

	/* the vertices and indices of one draw: the triangles side by side,
	 * row after row over the target (and round again) */
	{
		int cell = size > 1 ? (int)size + 1 : 2, cols = w / cell > 0 ? w / cell : 1;

		if (m <= 1) {
			nv = ni = n;
			verts = calloc(nv, sizeof(*verts));
			for (int k = 0; k < n / 3; k++) {
				float x = (k % cols) * cell, y = (k / cols * cell) % h;

				put(&verts[3 * k], x, y, w, h, behind, k);
				put(&verts[3 * k + 1], x + size, y, w, h, behind, k);
				put(&verts[3 * k + 2], x, y + size, w, h, behind, k);
			}
			if (m == 1) {
				idx = malloc(ni * sizeof(*idx));
				for (int i = 0; i < ni; i++)
					idx[i] = i;
			}
		} else if (m == 2) {
			/* strips of 32 triangles (34 vertices), each made a list:
			 * triangle t of a strip (t, t+1, t+2), the odd ones turned */
			int ntri = n / 3, nstrips = (ntri + 31) / 32;
			int sw = (int)(17 * size) + 1, scols = w / sw > 0 ? w / sw : 1;

			nv = nstrips * 34;
			ni = n;
			verts = calloc(nv, sizeof(*verts));
			idx = malloc(ni * sizeof(*idx));
			for (int s = 0; s < nstrips; s++) {
				float x0 = (s % scols) * sw, y0 = (int)((s / scols) * (size + 1)) % h;

				for (int i = 0; i < 34; i++)
					put(&verts[34 * s + i], x0 + (i / 2) * size, y0 + (i & 1) * size, w, h,
					    behind, s);
			}
			for (int k = 0; k < ntri; k++) {
				int s = k / 32, tt = k % 32, b = 34 * s;

				idx[3 * k] = b + tt + (tt & 1);
				idx[3 * k + 1] = b + tt + 1 - (tt & 1);
				idx[3 * k + 2] = b + tt + 2;
			}
		} else if (m == 3) {
			/* a grid of g x g quads, two triangles each, (g + 1)^2 vertices */
			int ntri = n / 3, g = 1;

			while (2 * (g + 1) * (g + 1) <= ntri)
				g++;
			nv = (g + 1) * (g + 1);
			ni = n;
			verts = calloc(nv, sizeof(*verts));
			idx = malloc(ni * sizeof(*idx));
			for (int j = 0; j <= g; j++)
				for (int i = 0; i <= g; i++)
					put(&verts[j * (g + 1) + i], (i * size), (float)((int)(j * size) % h), w,
					    h, behind, i + j);
			for (int k = 0; k < ntri; k++) {
				int q = (k / 2) % (g * g), qi = q % g, qj = q / g;
				int a = qj * (g + 1) + qi, b = a + 1, c = a + g + 1, d = c + 1;

				if (k & 1) {
					idx[3 * k] = b, idx[3 * k + 1] = d, idx[3 * k + 2] = c;
				} else {
					idx[3 * k] = a, idx[3 * k + 1] = b, idx[3 * k + 2] = c;
				}
			}
		} else {
			/* quads over the whole target, each a layer */
			static const int corner[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 },
			                                  { 0, 0 }, { 1, 1 }, { 0, 1 } };

			nv = ni = n;
			verts = calloc(nv, sizeof(*verts));
			for (int q = 0; q < n / 6; q++)
				for (int i = 0; i < 6; i++)
					put(&verts[6 * q + i], corner[i][0] * w, corner[i][1] * h, w, h, behind,
					    q * 3 + i);
		}
		if (nv > 65536)
			return fprintf(stderr, "glverts: %d vertices a draw, 65536 at most\n", nv), 2;
	}

	/* the shaders: nvary varyings, each from the colour and coordinates */
	{
		int k = 0, j = 0;

		k += snprintf(vs + k, sizeof(vs) - k,
		              "attribute vec3 p; attribute vec4 c; attribute vec2 uv;\n"
		              "uniform mat4 m;\n");
		j += snprintf(fs + j, sizeof(fs) - j, "precision mediump float;\n%s",
		              tex ? "uniform sampler2D s;\n" : "");
		for (int i = 0; i < nvary; i++) {
			k += snprintf(vs + k, sizeof(vs) - k, "varying vec4 v%d;\n", i);
			j += snprintf(fs + j, sizeof(fs) - j, "varying vec4 v%d;\n", i);
		}
		k += snprintf(vs + k, sizeof(vs) - k, "void main() {\n  gl_Position = m * vec4(p, 1.0);\n");
		j += snprintf(fs + j, sizeof(fs) - j, "void main() {\n  vec4 a = vec4(0.1, 0.2, 0.3, %s);\n",
		              nvary ? "0.0" : "1.0");
		for (int i = 0; i < nvary; i++) {
			if (i == 0)
				k += snprintf(vs + k, sizeof(vs) - k, "  v0 = c;\n");
			else if (i == 1)
				k += snprintf(vs + k, sizeof(vs) - k, "  v1 = vec4(uv, 0.0, 1.0);\n");
			else
				k += snprintf(vs + k, sizeof(vs) - k, "  v%d = c * %d.0 + vec4(uv, p.xy) * 0.5;\n",
				              i, i);
			if (i == 0)
				j += snprintf(fs + j, sizeof(fs) - j, "  a += v0;\n");
			else if (i == 1 && tex)
				j += snprintf(fs + j, sizeof(fs) - j, "  a *= texture2D(s, v1.xy);\n");
			else
				j += snprintf(fs + j, sizeof(fs) - j, "  a.rgb += v%d.rgb * 0.%d;\n", i, i + 1);
		}
		if (kill)
			j += snprintf(fs + j, sizeof(fs) - j, "  if (a.a < 0.5) discard;\n");
		snprintf(vs + k, sizeof(vs) - k, "}\n");
		snprintf(fs + j, sizeof(fs) - j, "  gl_FragColor = a;\n}\n");
	}

	get_display = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	dpy = get_display ? get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : NULL;
	if (!dpy || !eglInitialize(dpy, NULL, NULL) || !eglBindAPI(EGL_OPENGL_ES_API))
		return fprintf(stderr, "glverts: no EGL\n"), 2;
	ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ctx_attrs);
	if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
		return fprintf(stderr, "glverts: no GLES 2 context\n"), 2;

	/* the texture: the CPU's pixels, or a render's */
	if (tex) {
		uint8_t *img = malloc(tsize * tsize * 4);

		for (int i = 0; i < tsize * tsize * 4; i++)
			img[i] = (i & 3) == 3 ? 160 + (i * 13) % 96 : i * 7;
		glGenTextures(1, &t);
		glBindTexture(GL_TEXTURE_2D, t);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tsize, tsize, 0, GL_RGBA, GL_UNSIGNED_BYTE,
		             rtex ? NULL : img);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		if (rtex) {
			/* a render into it: a clear and a quad of colours */
			static const char *qvs = "attribute vec3 p; attribute vec4 c; varying vec4 vc;\n"
			                         "void main() { gl_Position = vec4(p, 1.0); vc = c; }\n";
			static const char *qfs = "precision mediump float; varying vec4 vc;\n"
			                         "void main() { gl_FragColor = vc; }\n";
			struct vtx q[6];
			GLuint tf, qp = program(qvs, qfs);

			for (int i = 0; i < 6; i++)
				put(&q[i], (i == 1 || i == 2 || i == 4) * tsize, (i == 2 || i >= 4) * tsize,
				    tsize, tsize, 0, i * 5);
			glGenFramebuffers(1, &tf);
			glBindFramebuffer(GL_FRAMEBUFFER, tf);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
			glViewport(0, 0, tsize, tsize);
			glClearColor(0.5f, 0.25f, 0.75f, 1);
			glClear(GL_COLOR_BUFFER_BIT);
			glUseProgram(qp);
			glEnableVertexAttribArray(0);
			glEnableVertexAttribArray(1);
			glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(q[0]), &q[0].x);
			glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(q[0]), q[0].c);
			glDrawArrays(GL_TRIANGLES, 0, 6);
			glFinish();
		}
		free(img);
	}

	glGenTextures(1, &rt);
	glBindTexture(GL_TEXTURE_2D, rt);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt, 0);
	if (depth) {
		glGenRenderbuffers(1, &zrb);
		glBindRenderbuffer(GL_RENDERBUFFER, zrb);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, zrb);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LEQUAL);
	}
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		return fprintf(stderr, "glverts: framebuffer incomplete\n"), 2;
	glViewport(0, 0, w, h);
	if (blend) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	if (mask)
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
	if (tex) {
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, t);
	}

	p = program(vs, fs);
	glUseProgram(p);
	{
		static const float id[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };

		glUniformMatrix4fv(glGetUniformLocation(p, "m"), 1, GL_FALSE, id);
		if (tex)
			glUniform1i(glGetUniformLocation(p, "s"), 0);
	}

	/* each draw its own vertices (no merging of one into the last: the
	 * next draw's are not after the last one's in the stream -- they are
	 * in a buffer of their own) */
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, (size_t)nv * sizeof(*verts) * draws, NULL, GL_DYNAMIC_DRAW);
	for (int d = 0; d < draws; d++)
		glBufferSubData(GL_ARRAY_BUFFER, (size_t)d * nv * sizeof(*verts), nv * sizeof(*verts), verts);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	if (idx) {
		glGenBuffers(1, &ibo);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, ni * sizeof(*idx), idx, GL_STATIC_DRAW);
	}

	for (int f = -1; f < frames; f++) {
		if (f == 0)
			t0 = now();     /* the first frame warms up: not counted */
		glClearColor(0, 0, 0.2f, 1);
		/* (the clear of every channel: the mask is the draws') */
		if (mask)
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glClear(GL_COLOR_BUFFER_BIT | (depth ? GL_DEPTH_BUFFER_BIT : 0));
		if (mask)
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
		if (cpu)
			for (int d = 0; d < draws; d++)
				glBufferSubData(GL_ARRAY_BUFFER, (size_t)d * nv * sizeof(*verts),
				                nv * sizeof(*verts), verts);
		for (int d = 0; d < draws; d++) {
			const char *b = (const char *)NULL + (size_t)d * nv * sizeof(*verts);

			glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(*verts), b);
			glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(*verts), b + 12);
			glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(*verts), b + 16);
			if (idx)
				glDrawElements(GL_TRIANGLES, ni, GL_UNSIGNED_SHORT, NULL);
			else
				glDrawArrays(GL_TRIANGLES, 0, nv);
		}
		glFinish();
	}
	el = now() - t0;
	printf("glverts -m %s -n %d -d %d -v %d -s %g%s%s%s%s%s%s%s -T %d%s%s: %d vertices a draw; "
	       "%.2f ms a frame, %.2f M indices a second\n", mode, ni, draws, nvary, size,
	       behind ? " -c" : "", depth ? " -z" : "", cpu ? " -q" : "", blend ? " -b" : "",
	       kill ? " -k" : "", mask ? " -M" : "", tex ? " -t" : "", tsize, rtex ? " -R" : "", nearest ? " -N" : "",
	       nv, el * 1000 / frames, (double)frames * draws * ni / el / 1e6);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	return 0;
}
