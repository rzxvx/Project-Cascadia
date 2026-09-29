/* kdtrace -- capture the kernel's kdebug trace from userspace (armv7, the
 * iPad's jailbroken iOS 8.4.1) and decode the AppleS5L8940XPerformanceController
 * events (debugid class 0x27).  READ ONLY, no tfp0, no kernel memory access --
 * kdebug is a normal sysctl (KERN_KDEBUG) available to root.
 *
 * Purpose: watch iOS's clock/voltage/perf state machine live. The perf
 * controller emits class-0x27 events whose schema we pulled from its IORegistry
 * "TraceBufferNomenclature": PERF_CLOCK_GATE{ClockID}, PERF_PERF_CHG_DOMx{...},
 * PERF_VOLT_CHG_DOMx{...}, etc. Running a GPU workload while tracing shows which
 * clock IDs get gated and which perf-states are entered when the SGX powers up --
 * the missing GFX/MANAGED enable sequence for the Linux clock bring-up.
 *
 *   kdtrace [ms]
 *     ms = capture window in milliseconds (default 1500). Run a GPU workload
 *          (e.g. gltrace) concurrently in another shell so its activity lands
 *          in the window:  /var/root/gltraceN & ; /var/root/kdtrace 2000
 *
 * Prints: a histogram of every debugid CLASS seen, then a detailed decode of
 * class-0x27 events (subclass name from the nomenclature, code, START/END, and
 * arg1..arg4 labelled by the schema).
 *
 * kdebug constants below are the stable xnu-2784 (iOS 8.4.1) ABI -- current SDKs
 * removed them from <sys/kdebug.h>, so they are inlined here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>                  /* uint64_t/uint32_t/uintptr_t (armv7: 4-byte) */

int sysctl(int *name, unsigned namelen, void *old, unsigned long *oldlen, void *newp, unsigned long newlen);
int usleep(unsigned);
void *malloc(unsigned long);
void free(void *);
int open(const char *, int, ...);
long write(int, const void *, unsigned long);
int fsync(int);
int close(int);
#define O_WRONLY 1
#define O_CREAT  0x200
#define O_TRUNC  0x400

#define CTL_KERN        1
#define KERN_KDEBUG     24            /* verified from the SDK's sys/sysctl.h */

/* KERN_KDEBUG sub-operations (mib[2]) */
#define KERN_KDEFLAGS   1
#define KERN_KDENABLE   3
#define KERN_KDSETBUF   4
#define KERN_KDGETBUF   5
#define KERN_KDSETUP    6
#define KERN_KDREMOVE   7
#define KERN_KDREADTR   10

typedef struct {                     /* K32 kd_buf: 32 bytes on armv7 */
    uint64_t  timestamp;
    uintptr_t arg1, arg2, arg3, arg4, arg5;   /* arg5 = thread */
    uint32_t  debugid;
} kd_buf;

typedef struct {
    int nkdbufs;
    int nolog;
    int flags;
    int nkdthreads;
    int bufid;
} kbufinfo_t;

#define DBGID_CLASS(d)    (((d) >> 24) & 0xff)
#define DBGID_SUBCLASS(d) (((d) >> 16) & 0xff)
#define DBGID_CODE(d)     (((d) >> 2)  & 0x3fff)
#define DBGID_FUNC(d)     ((d) & 3)          /* 1=START 2=END 0=none */

#define PERF_CLASS 0x27

/* From AppleS5L8940XPerformanceController's TraceBufferNomenclature (IOReg):
 * the 10 groups, in order, with their arg field labels. The group index is the
 * best guess for the debugid SUBCLASS; raw subclass/code are printed too so a
 * mismatch is obvious and correctable. */
static const char *perf_sub_name[] = {
    "PERF_PCEVENT", "PERF_CPU_IDLE", "PERF_CPU_IDLE_TIMER", "PERF_VOLT_CHG_DOMx",
    "PERF_PERF_CHG_DOMx", "PERF_CLOCK_GATE", "PERF_SRAMEMA_DOMx", "PERF_CPU_TICKS",
    "PERF_ARBITER_NOTIFY", "PERF_ARBITER_SET_PERF"
};
static const char *perf_sub_fields[] = {
    "",                                            /* PCEVENT */
    "",                                            /* CPU_IDLE */
    "",                                            /* CPU_IDLE_TIMER */
    "ReqVoltSt,DVCVoltSt,CurrentVoltSt,NewVoltSt", /* VOLT_CHG */
    "LimitVoltSt,ActualPerfSt,NewPerfSt",          /* PERF_CHG */
    "ClockID",                                     /* CLOCK_GATE  <-- GPU clock */
    "",                                            /* SRAMEMA */
    "",                                            /* CPU_TICKS */
    "",                                            /* ARBITER_NOTIFY */
    "voltLevel"                                    /* ARBITER_SET_PERF */
};
#define NPERF ((int)(sizeof(perf_sub_name)/sizeof(perf_sub_name[0])))

static int mib_op(int op, int arg, void *old, unsigned long *oldlen)
{
    int mib[4]; unsigned n = 3;
    mib[0] = CTL_KERN; mib[1] = KERN_KDEBUG; mib[2] = op;
    if (arg >= 0) { mib[3] = arg; n = 4; }
    return sysctl(mib, n, old, oldlen, 0, 0);
}

int main(int argc, char **argv)
{
    int ms = argc > 1 ? atoi(argv[1]) : 1500;
    /* argv[2]: a class in hex to detail-decode, OR "raw <path>" to dump the raw
     * kd_buf array to a file for offline analysis. 0 = all but noisy 01/04. */
    int rawmode = (argc > 2 && argv[2][0] == 'r');
    const char *rawpath = (rawmode && argc > 3) ? argv[3] : "/var/root/kd_raw.bin";
    unsigned want = (argc > 2 && !rawmode) ? (unsigned)strtoul(argv[2], 0, 16) : PERF_CLASS;
    int nbufs = 200000;                     /* ~6.4 MB of kd_buf */
    unsigned long len;

    mib_op(KERN_KDREMOVE, -1, 0, &len);     /* clear any prior session */
    if (mib_op(KERN_KDSETBUF, nbufs, 0, &len)) { printf("KDSETBUF failed\n"); return 1; }
    if (mib_op(KERN_KDSETUP,  -1,   0, &len)) { printf("KDSETUP failed\n");  return 1; }

    kbufinfo_t bi; len = sizeof bi;
    if (!mib_op(KERN_KDGETBUF, -1, &bi, &len))
        printf("kdebug ready: nkdbufs=%d flags=0x%x\n", bi.nkdbufs, bi.flags);
    if (bi.nkdbufs > 0) nbufs = bi.nkdbufs;

    mib_op(KERN_KDEFLAGS, 0, 0, &len);      /* default flags (wrap) */
    if (mib_op(KERN_KDENABLE, 1, 0, &len)) { printf("KDENABLE failed\n"); return 1; }

    if (ms > 0) usleep((unsigned)ms * 1000u);

    mib_op(KERN_KDENABLE, 0, 0, &len);      /* stop */

    kd_buf *buf = malloc((unsigned long)nbufs * sizeof(kd_buf));
    if (!buf) { printf("malloc %d entries failed\n", nbufs); mib_op(KERN_KDREMOVE,-1,0,&len); return 1; }
    len = (unsigned long)nbufs * sizeof(kd_buf);      /* input: bytes; output: entry COUNT */
    if (mib_op(KERN_KDREADTR, -1, buf, &len)) { printf("KDREADTR failed\n"); free(buf); mib_op(KERN_KDREMOVE,-1,0,&len); return 1; }
    long count = (long)len;                            /* kdebug returns #entries here */
    if (count > nbufs) count = count / (long)sizeof(kd_buf);   /* be robust if it were bytes */
    printf("captured %ld events\n", count);

    if (rawmode) {
        int fd = open(rawpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            long wrote = write(fd, buf, (unsigned long)count * sizeof(kd_buf));
            fsync(fd); close(fd);
            printf("raw dump: %ld bytes -> %s (kd_buf=%u each)\n", wrote, rawpath, (unsigned)sizeof(kd_buf));
        } else printf("raw dump: open(%s) failed\n", rawpath);
    }

    /* histogram of classes */
    long hist[256]; int c;
    for (c = 0; c < 256; c++) hist[c] = 0;
    for (long i = 0; i < count; i++) hist[DBGID_CLASS(buf[i].debugid)]++;
    printf("=== class histogram (nonzero) ===\n");
    for (c = 0; c < 256; c++) if (hist[c]) printf("  class 0x%02x : %ld%s\n", c, hist[c], c==PERF_CLASS?"  <-- perf controller":"");

    /* per-(subclass,code) histogram for the target class -- the key to the
     * ambient-vs-GPU-on diff: a subclass/code that appears (or jumps) only when
     * the GPU powers on is the GFX clock/power event. */
    if (want) {
        struct { unsigned key; long n; } sc[128]; int nsc = 0;
        for (long i = 0; i < count; i++) {
            uint32_t d = buf[i].debugid;
            if (DBGID_CLASS(d) != want) continue;
            unsigned key = (DBGID_SUBCLASS(d) << 16) | DBGID_CODE(d);
            int k; for (k = 0; k < nsc; k++) if (sc[k].key == key) { sc[k].n++; break; }
            if (k == nsc && nsc < 128) { sc[nsc].key = key; sc[nsc].n = 1; nsc++; }
        }
        printf("=== class 0x%02x (subclass,code) histogram ===\n", want);
        for (int k = 0; k < nsc; k++)
            printf("  sub=%-3u code=%-5u : %ld\n", (sc[k].key>>16)&0xff, sc[k].key&0xffff, sc[k].n);
    }

    /* detailed decode of the requested class (want==0: all but noisy 01/04) */
    if (want == 0) printf("=== detail: all classes except 0x01/0x04 ===\n");
    else           printf("=== class 0x%02x events ===\n", want);
    long shown = 0;
    for (long i = 0; i < count && shown < 2000; i++) {
        uint32_t d = buf[i].debugid;
        unsigned cl = DBGID_CLASS(d);
        if (want == 0) { if (cl == 0x01 || cl == 0x04) continue; }
        else if (cl != want) continue;
        int sub = (int)DBGID_SUBCLASS(d);
        int code = (int)DBGID_CODE(d);
        int fn = (int)DBGID_FUNC(d);
        const char *sname = (sub < NPERF) ? perf_sub_name[sub] : "?";
        const char *sfld  = (sub < NPERF) ? perf_sub_fields[sub] : "";
        printf("  [%ld] cl=0x%02x sub=%d(%s) code=%d %s a1=0x%x a2=0x%x a3=0x%x a4=0x%x %s%s\n",
               shown, cl, sub, sname, code,
               fn==1?"START":fn==2?"END":"-",
               (unsigned)buf[i].arg1, (unsigned)buf[i].arg2, (unsigned)buf[i].arg3, (unsigned)buf[i].arg4,
               sfld[0]?"; fields=":"", sfld);
        shown++;
    }
    if (shown == 0) printf("  (none -- perf events may need real load, or subclass differs)\n");

    free(buf);
    mib_op(KERN_KDREMOVE, -1, 0, &len);
    return 0;
}
