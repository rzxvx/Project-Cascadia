/* k93-thrash -- the iPad 2's pressure relief valve: when memory is gone and the
 * kernel keeps evicting the programs' own code only to read it straight back
 * (through the NAND server, ~10 MB/s at best), it asks the kernel's OOM killer
 * for one victim (sysrq f), as nothing else would.
 *
 * Why (2026-11-16): with the root and its page cache on the NAND server, a full
 * memory did not end in an OOM kill.  Each code page evicted counts as progress
 * for the kernel, so it went on evicting and re-reading code, minute after
 * minute, every process stalled and the OOM killer never called.  The multi-gen
 * LRU's min_ttl_ms ends that too, but at any useful setting it also killed
 * workloads zram carries well (a 300 MB fill at 1000 ms, a 400 MB random churn
 * at 200 ms).  What only the stuck case shows: MemAvailable near nothing while
 * file pages evicted moments ago keep faulting back (workingset_refault_file)
 * -- zram's churn refaults anonymous pages, not files.  Measured in the stuck
 * case: MemAvailable steady at ~32 MB (min_free_kbytes 8 MB keeps it there) and
 * 2,000-2,350 file refaults a second, the NAND server busy on both cores with
 * nothing but re-reads -- for 83 s and 165 s until the kernel's OOM killer
 * finally came on its own.
 *
 *   k93-thrash [-v]      every second; under FROM_MB (48) MemAvailable, over RATE
 *                        (800) file refaults/s, for SECONDS (10) running: one sysrq f,
 *                        then 10 s for the kill to free its memory.  -v: each
 *                        second's numbers to the kernel log while MemAvailable is
 *                        under 4 x FROM_MB.  Env K93THRASH="FROM_MB RATE SECONDS".
 *
 * Its own pages locked in RAM and SCHED_FIFO, so that it runs while everything
 * else waits on memory; no allocation after the start.  Started by
 * initramfs/init (its binary stays in the initramfs' RAM).
 *
 * Build: arm-linux-gnueabihf-gcc -static -O2 -Wall -o k93-thrash tools/k93-thrash.c
 */
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static char buf[16384];

/* the value after KEY (at a line's start) in FILE, or -1 */
static long long field(const char *file, const char *key)
{
    int fd = open(file, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = 0;
    size_t kl = strlen(key);
    for (char *p = buf; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
        if (!strncmp(p, key, kl) && (p[kl] == ' ' || p[kl] == ':'))
            return strtoll(p + kl + 1, NULL, 10);
    return -1;
}

static void say(const char *msg)
{
    int fd = open("/dev/kmsg", O_WRONLY);
    if (fd >= 0) {
        (void)!write(fd, msg, strlen(msg));
        close(fd);
    }
}

int main(int argc, char **argv)
{
    long from_mb = 48, rate = 800, secs = 10;
    int verbose = argc > 1 && !strcmp(argv[1], "-v");
    const char *e = getenv("K93THRASH");
    if (e)
        sscanf(e, "%ld %ld %ld", &from_mb, &rate, &secs);
    if (mlockall(MCL_CURRENT | MCL_FUTURE))
        perror("k93-thrash: mlockall");
    struct sched_param sp = { .sched_priority = 50 };
    if (sched_setscheduler(0, SCHED_FIFO, &sp))
        perror("k93-thrash: SCHED_FIFO");
    char msg[160];
    snprintf(msg, sizeof msg, "<6>k93-thrash: watching: MemAvailable < %ld MB with > %ld file refaults/s for %ld s "
             "-> an OOM kill\n", from_mb, rate, secs);
    say(msg);
    long long last = field("/proc/vmstat", "workingset_refault_file");
    long strike = 0;
    for (;;) {
        sleep(1);
        long long avail = field("/proc/meminfo", "MemAvailable");       /* kB */
        long long rf = field("/proc/vmstat", "workingset_refault_file");
        long long d = rf >= 0 && last >= 0 ? rf - last : 0;
        last = rf;
        if (avail >= 0 && avail < from_mb * 1024 && d > rate)
            strike++;
        else
            strike = 0;
        if (verbose && avail >= 0 && avail < 4 * from_mb * 1024) {   /* to the kernel log: it gets out */
            snprintf(msg, sizeof msg, "<6>k93-thrash: MemAvailable %lld kB, file refaults %lld/s, strike %ld\n",
                     avail, d, strike);
            say(msg);
        }
        if (strike >= secs) {
            snprintf(msg, sizeof msg, "<4>k93-thrash: %ld s of thrashing (MemAvailable %lld kB, %lld file refaults/s): "
                     "an OOM kill\n", strike, avail, d);
            say(msg);
            int fd = open("/proc/sysrq-trigger", O_WRONLY);
            if (fd >= 0) {
                (void)!write(fd, "f", 1);
                close(fd);
            }
            strike = 0;
            sleep(10);
            last = field("/proc/vmstat", "workingset_refault_file");
        }
    }
}
