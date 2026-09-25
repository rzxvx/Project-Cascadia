/* mtlog -- AppleMultitouchSPI's own trace, live, from iOS 8.4.1 on the iPad.
 *
 * The driver logs every step it takes -- power, reset, bootload, every report
 * it gets or sets, from the kernel or from userspace -- and a dump of every
 * SPI transfer up to 1034 bytes, commands, answers and frames alike.  With
 * boot-args mt-strings / mt-bytes it IOLogs them, but those need debug-enabled.
 * The same records also go to any user client that asks for them, with no
 * boot-arg at all (12H321, AppleMultitouchSPI kext):
 *
 *   log / dump      0x805ff6d4 / 0x805ff984: record if [driver+0x50] != 0,
 *                   the number of clients registered with flag 2
 *   registerClient  0x80600160: flag 2 -> [+0x50]++
 *   user client     IOExternalMethod table 0x8060cbc0, selector 3 (one
 *                   scalar, 1 on / 0 off) -> registerClient(this, 2)
 *                   (0x80602db4)
 *   the records     clientMemoryForType 0x10 (0x80603400): an IODataQueue,
 *                   created on first map, 0x200 entries
 *
 * Record: [0] kind (3 bytes, anything else text -- the log level), [1] [2] args, [4..11] timestamp
 * {u32 s, u32 us},
 * [0xc] u16 text or TX length, [0xe] u16 RX length, [0x10..] text, or TX
 * then RX bytes.
 *
 * The digitizer's personality has DisablePowerForUILock and
 * ResetWhenExitingUILock: lock the iPad and unlock it, and the driver powers
 * the chip down, resets it and bootloads it again -- all of it into this log.
 *
 *   mtlog [seconds]        default 60; stdout, one line per record
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>

typedef unsigned int io_object_t;
typedef unsigned int io_connect_t;
typedef unsigned int mach_port_t;
typedef int kern_return_t;
extern mach_port_t mach_task_self_;
kern_return_t IOObjectRelease(io_object_t);
CFMutableDictionaryRef IOServiceMatching(const char *name);
io_object_t IOServiceGetMatchingService(mach_port_t master, CFDictionaryRef matching);
kern_return_t IOServiceOpen(io_object_t service, mach_port_t task, uint32_t type, io_connect_t *conn);
kern_return_t IOServiceClose(io_connect_t conn);
kern_return_t IOConnectMapMemory64(io_connect_t conn, uint32_t type, mach_port_t task,
                                   uint64_t *addr, uint64_t *size, uint32_t options);
kern_return_t IOConnectCallScalarMethod(io_connect_t conn, uint32_t selector,
                                        const uint64_t *in, uint32_t nin,
                                        uint64_t *out, uint32_t *nout);
int IODataQueueDataAvailable(void *queue);
kern_return_t IODataQueueDequeue(void *queue, void *data, uint32_t *size);

struct mach_timebase_info { uint32_t numer, denom; };
kern_return_t mach_timebase_info(struct mach_timebase_info *);
uint64_t mach_absolute_time(void);

static void hexline(const char *tag, const uint8_t *p, unsigned n)
{
    unsigned i;

    printf(" %s %u:", tag, n);
    for(i = 0; i < n; i++)
        printf(" %02x", p[i]);
}

int main(int argc, char **argv)
{
    static uint8_t rec[0x1000];
    struct mach_timebase_info tb;
    io_object_t svc;
    io_connect_t conn = 0;
    uint64_t addr = 0, size = 0, on = 1, off = 0, t, t0 = 0, end;
    uint32_t n, kind, type;
    unsigned secs = argc > 1 ? (unsigned)atoi(argv[1]) : 60, nrec = 0;
    kern_return_t kr;
    void *q;

    setvbuf(stdout, NULL, _IOLBF, 0);
    mach_timebase_info(&tb);

    svc = IOServiceGetMatchingService(0, IOServiceMatching("AppleMultitouchN1SPI"));
    if(!svc) {
        printf("no AppleMultitouchN1SPI\n");
        return 1;
    }
    for(type = 0; type < 4; type++) {
        kr = IOServiceOpen(svc, mach_task_self_, type, &conn);
        if(!kr)
            break;
        printf("IOServiceOpen type %u: 0x%08x\n", type, kr);
    }
    IOObjectRelease(svc);
    if(kr)
        return 1;
    printf("user client open (type %u)\n", type);

    kr = IOConnectMapMemory64(conn, 0x10, mach_task_self_, &addr, &size, 1 /* kIOMapAnywhere */);
    if(kr) {
        printf("map log queue: 0x%08x\n", kr);
        return 1;
    }
    q = (void *)(uintptr_t)addr;
    printf("log queue at 0x%llx, %llu bytes\n", addr, size);

    kr = IOConnectCallScalarMethod(conn, 3, &on, 1, NULL, NULL);
    printf("selector 3 (logging on): 0x%08x\n", kr);
    if(kr)
        return 1;
    printf("== logging for %u s -- lock and unlock the iPad, touch the glass\n", secs);

    end = mach_absolute_time() + (uint64_t)secs * 1000000000ULL * tb.denom / tb.numer;
    while(mach_absolute_time() < end) {
        if(!IODataQueueDataAvailable(q)) {
            usleep(2000);
            continue;
        }
        n = sizeof(rec);
        if(IODataQueueDequeue(q, rec, &n) || n < 0x10)
            continue;
        nrec++;
        t = (uint64_t)(rec[4] | rec[5] << 8 | rec[6] << 16 | (uint32_t)rec[7] << 24) * 1000000 +
            (rec[8] | rec[9] << 8 | rec[10] << 16 | (uint32_t)rec[11] << 24);
        if(!t0)
            t0 = t;
        kind = rec[0];
        printf("%9.3f ", (double)(t - t0) / 1000);
        if(kind != 3) {
            unsigned len = rec[0xc] | rec[0xd] << 8;
            if(len > n - 0x10)
                len = n - 0x10;
            while(len && (rec[0x10 + len - 1] == '\n' || rec[0x10 + len - 1] == 0))
                len--;
            if(kind != 1)
                printf("[%u] ", kind);
            printf("%.*s\n", (int)len, rec + 0x10);
        } else {
            unsigned tx = rec[0xc] | rec[0xd] << 8, rx = rec[0xe] | rec[0xf] << 8;
            if(0x10 + tx + rx > n) {
                printf("bytes? tx %u rx %u in %u\n", tx, rx, n);
                continue;
            }
            printf("SPI");
            hexline("TX", rec + 0x10, tx);
            hexline("RX", rec + 0x10 + tx, rx);
            printf("\n");
        }
    }

    IOConnectCallScalarMethod(conn, 3, &off, 1, NULL, NULL);
    IOServiceClose(conn);
    printf("== %u records\n", nrec);
    return 0;
}
