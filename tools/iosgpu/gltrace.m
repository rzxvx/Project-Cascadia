// gltrace -- drive OpenGL ES 2 on the iPad's own jailbroken iOS 8.4.1 and
// capture what the GL driver (IMGSGX543GLDriver) submits to the kernel, so the
// SGX543 command stream can be reverse engineered without a driver of our own.
// See docs/research/p105-gpu.md.  Read only: it renders offscreen and logs.
//
// It runs three frames into one offscreen FBO, changing as little as possible
// between them so the command buffer can be diffed:
//   frame a: clear only          (baseline)
//   frame b: one triangle, uColor = orange
//   frame c: same triangle, uColor = teal   (only a uniform changes)
// After each it writes the full contents of every shared buffer the driver
// mapped (IOConnectMapMemory) to /var/root/gt_<a|b|c>_map<type>.bin.  Diffing
// a vs b isolates the draw (VDM/state/USSE/PDS); b vs c isolates the colour
// constant.  Every IOConnectCall* into the kernel is logged to stdout.
//
// Build: tools/iosgpu/build.sh (macOS + Xcode; stubs/*.tbd, ldid, no entitlements).
#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES2/gl.h>
#import <OpenGLES/ES2/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include "fishhook.h"

typedef unsigned int mach_port_t;
typedef int kern_return_t;
typedef unsigned int vm_address_t;
typedef unsigned int vm_size_t;

long write(int, const void *, unsigned long);
int close(int);   /* open() and the O_* flags come from <fcntl.h> */
void *malloc(unsigned long);
void free(void *);
extern mach_port_t mach_task_self_;
#define mach_task_self() mach_task_self_
kern_return_t vm_read_overwrite(mach_port_t, vm_address_t, vm_size_t, vm_address_t, vm_size_t *);

/* the CPU arena the GL driver keeps its GPU-shared buffers in (map0/1/2 and the
 * submit structs' pointers all landed here); dumped as a sparse image so the
 * same file offset is the same address across frames a/b/c */
#define ARENA_LO 0x00400000u
#define ARENA_HI 0x00c00000u

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
    printf("[call]        conn %u sel %-4u scalars %u struct %zu\n", c, sel, inc, insc);
    if (ins && insc)
        dump("in", ins, insc, 128);
    return orig_call(c, sel, in, inc, ins, insc, out, outc, outs, outsc);
}

static kern_return_t my_call_struct(mach_port_t c, uint32_t sel, const void *ins, size_t insc,
                                    void *outs, size_t *outsc)
{
    printf("[callStruct]  conn %u sel %-4u struct %zu\n", c, sel, insc);
    if (ins && insc)
        dump("in", ins, insc, 128);
    return orig_call_struct(c, sel, ins, insc, outs, outsc);
}

static kern_return_t my_call_scalar(mach_port_t c, uint32_t sel, const uint64_t *in, uint32_t inc,
                                    uint64_t *out, uint32_t *outc)
{
    printf("[callScalar]  conn %u sel %-4u scalars %u\n", c, sel, inc);
    return orig_call_scalar(c, sel, in, inc, out, outc);
}

static kern_return_t my_map(mach_port_t c, uint32_t type, mach_port_t task,
                            vm_address_t *addr, vm_size_t *size, uint32_t opts)
{
    kern_return_t r = orig_map(c, type, task, addr, size, opts);
    if (r == 0 && addr && size) {
        printf("[map]         conn %u type %u -> 0x%08x + 0x%x\n", c, type, *addr, *size);
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

/* write every mapped buffer to /var/root/gt_<label>_map<type>.bin */
static void snapshot(const char *label)
{
    for (int i = 0; i < nmaps; i++) {
        char path[64];
        snprintf(path, sizeof path, "/var/root/gt_%s_map%u.bin", label, maps[i].type);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, (const void *)(uintptr_t)maps[i].addr, maps[i].size);
            close(fd);
        }
    }
    printf("== snapshot '%s': %d buffers written\n", label, nmaps);
}

/* dump the CPU arena as a sparse image: mapped pages copied in, holes left zero */
static void snapshot_arena(const char *label)
{
    unsigned span = ARENA_HI - ARENA_LO;
    unsigned char *img = malloc(span);
    if (!img)
        return;
    memset(img, 0, span);
    unsigned mapped = 0;
    for (unsigned off = 0; off < span; off += 0x1000) {
        vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(), ARENA_LO + off, 0x1000,
                              (vm_address_t)(img + off), &got) == 0 && got == 0x1000)
            mapped++;
    }
    char path[64];
    snprintf(path, sizeof path, "/var/root/gt_%s_arena.bin", label);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, img, span);
        close(fd);
    }
    free(img);
    printf("== arena '%s': %u/%u pages mapped\n", label, mapped, span / 0x1000);
}

/* ---- GLES ------------------------------------------------------------- */

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

int main(void)
{
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

    const char *vs = "attribute vec4 p; void main(){ gl_Position = p; }";
    /* two fragment programs differing by ONE op: prog1 outputs the uniform,
     * prog2 outputs uColor*uColor.  Both are compiled and drawn in this one
     * process, so both USSE programs sit in the arena at once; the two are
     * near-identical (that is our fragment program) and their one-instruction
     * difference is the MUL -- a first operand-level decode. */
    const char *fs1 = "precision mediump float; uniform vec4 uColor;"
                      "void main(){ gl_FragColor = uColor; }";
    const char *fs2 = "precision mediump float; uniform vec4 uColor;"
                      "void main(){ gl_FragColor = uColor * uColor; }";
    GLuint vsh = make_shader(GL_VERTEX_SHADER, vs);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vsh);
    glAttachShader(prog, make_shader(GL_FRAGMENT_SHADER, fs1));
    glBindAttribLocation(prog, 0, "p");
    glLinkProgram(prog);
    GLuint prog2 = glCreateProgram();
    glAttachShader(prog2, vsh);
    glAttachShader(prog2, make_shader(GL_FRAGMENT_SHADER, fs2));
    glBindAttribLocation(prog2, 0, "p");
    glLinkProgram(prog2);
    glUseProgram(prog);
    GLint uColor = glGetUniformLocation(prog, "uColor");

    static const float tri[] = { 0.0f, 0.6f, -0.6f, -0.6f, 0.6f, -0.6f };
    glViewport(0, 0, 64, 64);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, tri);

    /* a: clear only */
    printf("== frame a (clear only)\n");
    glClearColor(0, 0, 0.2f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    snapshot("a"); snapshot_arena("a");

    /* b: triangle, orange */
    printf("== frame b (triangle, orange)\n");
    glClear(GL_COLOR_BUFFER_BIT);
    glUniform4f(uColor, 1.0f, 0.5f, 0.0f, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    snapshot("b"); snapshot_arena("b");

    /* c: triangle with prog2 (uColor*uColor) -- a different fragment program */
    printf("== frame c (triangle, prog2 = uColor*uColor)\n");
    glUseProgram(prog2);
    glUniform4f(glGetUniformLocation(prog2, "uColor"), 1.0f, 0.5f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    snapshot("c"); snapshot_arena("c");

    unsigned char px[4] = { 0 };
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("== centre pixel after c: %02x %02x %02x %02x (prog2: uColor^2 -> ~ff 40 00 ff)\n",
           px[0], px[1], px[2], px[3]);
    return 0;
}
