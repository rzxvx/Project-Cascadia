/* hfs-resize -- grow or shrink a mounted HFS+ volume on the iPad's own iOS
 * (the kernel's HFSIOC_RESIZE_VOLUME fsctl, what CoolBooter-style dual boots
 * use to make room for new LwVM partitions).  Used by tools/k93-part.sh.
 *
 *   hfs-resize probe PATH         fsctl(PATH, HFSIOC_RESIZE_PROGRESS): shows
 *                                 that the call reaches HFS; changes nothing
 *   hfs-resize resize PATH BYTES  fsctl(PATH, HFSIOC_RESIZE_VOLUME, BYTES):
 *                                 PATH must be the volume's root (/private/var),
 *                                 BYTES a multiple of its block size
 *
 * No libc and no Apple toolchain: freestanding ARM code built by the Linux
 * cross compiler, XNU system calls by hand (number in r12, svc 0x80, the
 * carry flag set on error with the errno in r0), wrapped into a static
 * Mach-O by tools/ios/mkmacho.py and signed on the iPad with its own ldid.
 *
 * Build (from devices/a5, in the cascadia-build image): tools/ios/build.sh
 */
typedef unsigned int u32;
typedef unsigned long long u64;

#define SYS_exit        1
#define SYS_write       4
#define SYS_fsctl       242

/* bsd/hfs/hfs_fsctl.h: HFSIOC_RESIZE_PROGRESS = _IOR('h', 1, u_int32_t),
 * HFSIOC_RESIZE_VOLUME = _IOW('h', 2, u_int64_t).  ('h' 3 and 4 are
 * CHANGE_NEXT_ALLOCATION and GETCREATETIME; fsctl hands HFS the command with
 * its size bits stripped, so the direction bits must match too.)  The probe
 * answers ENXIO when no resize is running: that checks the number before a
 * resize is ever sent. */
#define HFSIOC_RESIZE_PROGRESS  0x40046801u
#define HFSIOC_RESIZE_VOLUME    0x80086802u
#define ENXIO                   6

static int xsys(int n, int a, int b, int c, int d, int *err)
{
    register int r0 __asm__("r0") = a;
    register int r1 __asm__("r1") = b;
    register int r2 __asm__("r2") = c;
    register int r3 __asm__("r3") = d;
    register int r12 __asm__("r12") = n;
    int cs;
    __asm__ volatile("svc #0x80\n\tmovcs %1, #1\n\tmovcc %1, #0"
                     : "+r"(r0), "=r"(cs), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r12)
                     :
                     : "memory", "cc", "lr");
    *err = cs ? r0 : 0;
    return cs ? -1 : r0;
}

static u32 slen(const char *s)
{
    u32 n = 0;
    while (s[n])
        n++;
    return n;
}

static void out(const char *s)
{
    int e;
    xsys(SYS_write, 1, (int)s, (int)slen(s), 0, &e);
}

static void outu(u64 v)
{
    char b[24];
    int i = 23;
    b[i] = 0;
    do {
        u32 lo = (u32)(v % 10);
        b[--i] = (char)('0' + lo);
        v /= 10;
    } while (v);
    out(b + i);
}

static int parse(const char *s, u64 *v)
{
    *v = 0;
    if (!*s)
        return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return -1;
        *v = *v * 10 + (u64)(*s - '0');
    }
    return 0;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

int cmain(int argc, char **argv)
{
    int e;
    if (argc == 3 && streq(argv[1], "probe")) {
        u32 progress = 0xffffffffu;
        int r = xsys(SYS_fsctl, (int)argv[2], (int)HFSIOC_RESIZE_PROGRESS, (int)&progress, 0, &e);
        out("probe ");
        out(argv[2]);
        if (r < 0 && e == ENXIO) {
            out(": ok, HFS answers (no resize running)");
        } else if (r < 0) {
            out(": errno ");
            outu((u32)e);
        } else {
            out(": ok, progress ");
            outu(progress);
        }
        out("\n");
        return 0;
    }
    if (argc == 4 && streq(argv[1], "resize")) {
        u64 size;
        if (parse(argv[3], &size) || size == 0) {
            out("hfs-resize: BYTES must be a decimal number\n");
            return 2;
        }
        out("resizing ");
        out(argv[2]);
        out(" to ");
        outu(size);
        out(" bytes...\n");
        int r = xsys(SYS_fsctl, (int)argv[2], (int)HFSIOC_RESIZE_VOLUME, (int)&size, 0, &e);
        if (r < 0) {
            out("hfs-resize: fsctl failed, errno ");
            outu((u32)e);
            out("\n");
            return 1;
        }
        out("hfs-resize: done\n");
        return 0;
    }
    out("usage: hfs-resize probe PATH | resize PATH BYTES\n");
    return 2;
}

/* XNU starts a static image with sp -> argc, argv[0..], 0, envp... */
__asm__(".text\n"
        ".arm\n"
        ".globl _start\n"
        "_start:\n"
        "    ldr r0, [sp]\n"
        "    add r1, sp, #4\n"
        "    bic sp, sp, #7\n"
        "    bl cmain\n"
        "    mov r12, #1\n"
        "    svc #0x80\n"
        "1:  b 1b\n");
