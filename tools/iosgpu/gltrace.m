// gltrace -- draw a triangle with OpenGL ES 2, offscreen, on the iPad's own
// jailbroken iOS 8.4.1, and log every call the GL driver makes into the kernel
// (IOConnectCall*), plus the shared buffers it maps (IOConnectMapMemory).
//
// This is how the SGX543 command stream gets reverse engineered without a GPU
// driver of our own: iOS's IMGSGX543GLDriver builds the real USSE/PDS/command
// stream in shared memory and submits it through an IOAccelerator user client;
// being the rendering process, we fishhook those calls and dump what goes by.
// See docs/research/p105-gpu.md.  Read only -- it renders and logs, nothing else.
//
//   gltrace [frames]        default 1; dumps to stdout
//
// Build: tools/iosgpu/build.sh (macOS + Xcode; links against stubs/*.tbd for
// armv7-ios, fake-signed with ldid).
#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES2/gl.h>
#import <OpenGLES/ES2/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fishhook.h"

typedef unsigned int mach_port_t;
typedef int kern_return_t;
typedef unsigned int vm_address_t;
typedef unsigned int vm_size_t;

/* ---- the IOKit calls we intercept ------------------------------------- */

static kern_return_t (*orig_call)(mach_port_t, uint32_t, const uint64_t *, uint32_t,
                                  const void *, size_t, uint64_t *, uint32_t *,
                                  void *, size_t *);
static kern_return_t (*orig_call_struct)(mach_port_t, uint32_t, const void *, size_t,
                                         void *, size_t *);
static kern_return_t (*orig_call_scalar)(mach_port_t, uint32_t, const uint64_t *, uint32_t,
                                         uint64_t *, uint32_t *);
static kern_return_t (*orig_map)(mach_port_t, uint32_t, mach_port_t,
                                 vm_address_t *, vm_size_t *, uint32_t);

/* the buffers the driver maps to share with the GPU/kernel; dumped at the end */
static struct { vm_address_t addr; vm_size_t size; uint32_t type; } maps[64];
static int nmaps;

static void dump(const char *tag, const unsigned char *p, size_t n, size_t cap)
{
    size_t m = n < cap ? n : cap;
    printf("    %s (%zu bytes):", tag, n);
    for (size_t i = 0; i < m; i++) {
        if (i % 16 == 0)
            printf("\n      %04zx:", i);
        printf(" %02x", p[i]);
    }
    if (n > m)
        printf(" ...");
    printf("\n");
}

static kern_return_t my_call(mach_port_t c, uint32_t sel, const uint64_t *in, uint32_t inc,
                             const void *ins, size_t insc, uint64_t *out, uint32_t *outc,
                             void *outs, size_t *outsc)
{
    printf("[IOConnectCallMethod] conn %u sel %u  scalars %u  struct %zu\n", c, sel, inc, insc);
    for (uint32_t i = 0; i < inc && i < 8; i++)
        printf("      s[%u] = 0x%llx\n", i, (unsigned long long)in[i]);
    if (ins && insc)
        dump("in-struct", ins, insc, 256);
    return orig_call(c, sel, in, inc, ins, insc, out, outc, outs, outsc);
}

static kern_return_t my_call_struct(mach_port_t c, uint32_t sel, const void *ins, size_t insc,
                                    void *outs, size_t *outsc)
{
    printf("[IOConnectCallStructMethod] conn %u sel %u  struct %zu\n", c, sel, insc);
    if (ins && insc)
        dump("in-struct", ins, insc, 256);
    return orig_call_struct(c, sel, ins, insc, outs, outsc);
}

static kern_return_t my_call_scalar(mach_port_t c, uint32_t sel, const uint64_t *in, uint32_t inc,
                                    uint64_t *out, uint32_t *outc)
{
    printf("[IOConnectCallScalarMethod] conn %u sel %u  scalars %u\n", c, sel, inc);
    for (uint32_t i = 0; i < inc && i < 8; i++)
        printf("      s[%u] = 0x%llx\n", i, (unsigned long long)in[i]);
    return orig_call_scalar(c, sel, in, inc, out, outc);
}

static kern_return_t my_map(mach_port_t c, uint32_t type, mach_port_t task,
                            vm_address_t *addr, vm_size_t *size, uint32_t opts)
{
    kern_return_t r = orig_map(c, type, task, addr, size, opts);
    if (r == 0 && addr && size) {
        printf("[IOConnectMapMemory] conn %u type %u -> 0x%08x + 0x%x\n", c, type, *addr, *size);
        if (nmaps < 64) {
            maps[nmaps].addr = *addr;
            maps[nmaps].size = *size;
            maps[nmaps].type = type;
            nmaps++;
        }
    }
    return r;
}

static void install_hooks(void)
{
    struct rebinding r[] = {
        { "IOConnectCallMethod", my_call, (void **)&orig_call },
        { "IOConnectCallStructMethod", my_call_struct, (void **)&orig_call_struct },
        { "IOConnectCallScalarMethod", my_call_scalar, (void **)&orig_call_scalar },
        { "IOConnectMapMemory", my_map, (void **)&orig_map },
    };
    rebind_symbols(r, sizeof(r) / sizeof(r[0]));
}

/* ---- a triangle ------------------------------------------------------- */

static GLuint make_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, 0);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, 0, log);
        printf("shader error: %s\n", log);
    }
    return s;
}

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 1;

    install_hooks();
    printf("== hooks installed; creating GLES2 context\n");

    id ctx = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
    if (!ctx || ![EAGLContext setCurrentContext:ctx]) {
        printf("no GLES2 context -- headless EAGL may be denied on this build\n");
        return 1;
    }
    printf("== context ok, renderer \"%s\"\n", (const char *)glGetString(GL_RENDERER));

    GLuint tex, fbo;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        printf("FBO incomplete (0x%x)\n", glCheckFramebufferStatus(GL_FRAMEBUFFER));

    const char *vs = "attribute vec4 p; void main(){ gl_Position = p; }";
    const char *fs = "precision mediump float; void main(){ gl_FragColor = vec4(1.0,0.5,0.0,1.0); }";
    GLuint prog = glCreateProgram();
    glAttachShader(prog, make_shader(GL_VERTEX_SHADER, vs));
    glAttachShader(prog, make_shader(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(prog, 0, "p");
    glLinkProgram(prog);
    glUseProgram(prog);

    static const float tri[] = { 0.0f, 0.6f, -0.6f, -0.6f, 0.6f, -0.6f };
    glViewport(0, 0, 64, 64);

    for (int f = 0; f < frames; f++) {
        printf("== frame %d\n", f);
        glClearColor(0, 0, 0.2f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glFinish();
    }

    unsigned char px[4] = { 0 };
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("== centre pixel: %02x %02x %02x %02x (expect ~ff 80 00 ff if the GPU drew)\n",
           px[0], px[1], px[2], px[3]);

    printf("== %d shared mappings; dumping their heads\n", nmaps);
    for (int i = 0; i < nmaps; i++) {
        char tag[48];
        snprintf(tag, sizeof tag, "map type %u @ 0x%08x", maps[i].type, maps[i].addr);
        dump(tag, (const unsigned char *)(uintptr_t)maps[i].addr, maps[i].size, 512);
    }
    return 0;
}
