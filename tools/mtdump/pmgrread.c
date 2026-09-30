/* pmgrread -- read the live PMGR clock registers of the S5L8942X while the GPU
 * is powered, to recover the GFX/MANAGED source+divider values the Linux clock
 * driver must program. armv7, the iPad's jailbroken iOS 8.4.1. READ ONLY.
 *
 * HAL/JTAG-substitute hardware debugging for the GPL Linux driver: on Linux the
 * GPU is off and the PMGR clock-config regs read back "disabled", so the enabled
 * values are only observable on the running system. This reads them.
 *
 * Safe by construction -- NO blind kernel scan:
 *   1. get the io_service for AppleS5L8940XPerformanceController (IOKit);
 *   2. mach_port_kobject() returns that IOService object's KERNEL ADDRESS
 *      directly (no memory access needed to find it);
 *   3. task_for_pid(0) gives the kernel task; vm_read reads the object -- it is
 *      live kernel heap, so it is mapped and cannot fault;
 *   4. its register-base ivar (_pcBaseAddress, per Ghidra at +0x550) is the
 *      kernel VA that maps PMGR (phys 0x3f100000); reading through it hits the
 *      MMIO. PMGR is always powered, so unlike the SGX it does not hang.
 *
 *   pmgrread obj                dump the perf-controller object (find the base VA)
 *   pmgrread pmgr <kernelVA>    read the clock region / perf-state table / gates
 *                               through that base VA (run only after 'obj')
 *
 * Needs the task_for_pid-allow entitlement (sign with tfp0.entitlements).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

typedef unsigned int mach_port_t;
typedef int kern_return_t;
typedef unsigned int vm_size_t;
typedef unsigned int vm_address_t;
#define KERN_SUCCESS 0

extern mach_port_t mach_task_self_;
#define mach_task_self() mach_task_self_

kern_return_t task_for_pid(mach_port_t target, int pid, mach_port_t *t);
kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t addr, vm_size_t size,
                                vm_address_t data, vm_size_t *out);
char *mach_error_string(kern_return_t);
kern_return_t vm_region_recurse(mach_port_t task, vm_address_t *address, vm_size_t *size,
                                unsigned int *nesting_depth, int *info, unsigned int *infoCnt);
void *IOServiceMatching(const char *name);
kern_return_t IOServiceGetMatchingServices(mach_port_t master, void *matching, mach_port_t *iter);
mach_port_t IOIteratorNext(mach_port_t iter);
kern_return_t IOObjectGetClass(mach_port_t obj, char *name);
kern_return_t IOObjectRelease(mach_port_t obj);
/* returns the kernel address of the object backing a mach port -- no memory read */
kern_return_t mach_port_kobject(mach_port_t task, mach_port_t name,
                                unsigned int *object_type, unsigned long long *object_addr);

#define PC_BASE_IVAR 0x550          /* _pcBaseAddress in the perf controller (Ghidra) */

static mach_port_t kt;
static int logfd = -1;

static void logln(const char *s)
{
    fputs(s, stdout); fputc('\n', stdout); fflush(stdout);
    if (logfd >= 0) { write(logfd, s, strlen(s)); write(logfd, "\n", 1); fsync(logfd); }
}

static int kread(vm_address_t va, void *buf, vm_size_t n)
{
    vm_size_t got = 0;
    return vm_read_overwrite(kt, va, n, (vm_address_t)buf, &got) == KERN_SUCCESS && got == n;
}

/* dump n bytes at va as 32-bit words, 4 per line, with the VA on the left */
static void dump_words(vm_address_t va, const unsigned int *w, vm_size_t n)
{
    char b[128];
    for (vm_size_t i = 0; i < n/4; i += 4) {
        int p = snprintf(b, sizeof b, "  %08x:", va + i*4);
        for (vm_size_t j = i; j < i+4 && j < n/4; j++)
            p += snprintf(b+p, sizeof b-p, " %08x", w[j]);
        logln(b);
    }
}

static mach_port_t find_perf_controller(void)
{
    mach_port_t it = 0, e;
    if (IOServiceGetMatchingServices(0, IOServiceMatching("AppleS5L8940XPerformanceController"), &it) || !it)
        return 0;
    e = IOIteratorNext(it);
    IOObjectRelease(it);
    return e;
}

int main(int argc, char **argv)
{
    logfd = open("/var/root/pmgrread.log", O_WRONLY|O_CREAT|O_TRUNC, 0644);
    logln("== pmgrread start ==");

    kern_return_t kr = task_for_pid(mach_task_self(), 0, &kt);
    if (kr != KERN_SUCCESS || !kt) { char b[128]; snprintf(b,sizeof b,"task_for_pid(0) failed: %s", mach_error_string(kr)); logln(b); return 2; }
    { char b[64]; snprintf(b,sizeof b,"kernel task port %u", kt); logln(b); }

    if (argc >= 2 && strcmp(argv[1], "kmap") == 0) {
        /* enumerate the kernel VM map (vm_region_recurse -- query-only, no region
         * reads, reboot-safe). struct vm_region_submap_info, 19 ints. */
        struct submap_info {
            unsigned int protection, max_protection, inheritance, offset, user_tag,
                         pages_resident, pages_shared_now_private, pages_swapped_out,
                         pages_dirtied, ref_count;
            unsigned short shadow_depth; unsigned char external_pager, share_mode;
            unsigned int is_submap, behavior, object_id; unsigned short user_wired_count;
        };
        vm_address_t addr = 0x80000000; unsigned int n = 0; char b[200];
        logln("  addr       size       prot mx tag   shr sub depth");
        for (;;) {
            vm_size_t size = 0; unsigned int depth = 100, cnt = 15;
            struct submap_info info;
            kern_return_t rr = vm_region_recurse(kt, &addr, &size, &depth, (int *)&info, &cnt);
            if (rr != KERN_SUCCESS) {
                if (n == 0) { snprintf(b,sizeof b,"vm_region_recurse kr=%d (%s) cnt_out=%u", rr, mach_error_string(rr), cnt); logln(b); }
                break;
            }
            snprintf(b, sizeof b, "  %08x  %08x   %x  %x  %-4u  %u   %u   %u",
                     addr, size, info.protection, info.max_protection, info.user_tag,
                     info.share_mode, info.is_submap, depth);
            logln(b);
            if (++n > 4000) { logln("  ...(capped)"); break; }
            addr += size; if (!addr) break;
        }
        { char e[64]; snprintf(e,sizeof e,"== %u regions ==", n); logln(e); }
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "obj") == 0) {
        mach_port_t obj = find_perf_controller();
        if (!obj) { logln("perf controller service not found"); return 1; }
        char cls[128] = ""; IOObjectGetClass(obj, cls);
        { char b[160]; snprintf(b,sizeof b,"service io_object=%u class=%s", obj, cls); logln(b); }

        unsigned int type = 0; unsigned long long kaddr = 0;
        kr = mach_port_kobject(mach_task_self(), obj, &type, &kaddr);
        if (kr != KERN_SUCCESS) { char b[128]; snprintf(b,sizeof b,"mach_port_kobject failed: %s", mach_error_string(kr)); logln(b); return 1; }
        { char b[128]; snprintf(b,sizeof b,"kobject type=0x%x addr=0x%llx", type, kaddr); logln(b); }

        vm_address_t oa = (vm_address_t)kaddr;
        static unsigned int w[0x600/4];
        if (!kread(oa, w, sizeof w)) { logln("vm_read of the object failed"); return 1; }
        logln("== object dump (look for a device-mapping VA; _pcBaseAddress ~ +0x550) ==");
        dump_words(oa, w, sizeof w);
        { char b[96]; snprintf(b,sizeof b,"word @ +0x550 (pcBase candidate) = 0x%08x", w[PC_BASE_IVAR/4]); logln(b); }
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "pmgr") == 0) {
        vm_address_t base = (vm_address_t)strtoul(argv[2], 0, 16);
        { char b[96]; snprintf(b,sizeof b,"PMGR base VA = 0x%08x -- reading clock region", base); logln(b); }
        static unsigned int w[0x100/4];
        if (kread(base + 0x00, w, sizeof w)) { logln("-- clock config 0x00..0xff --"); dump_words(base+0x00, w, sizeof w); }
        else logln("read of clock region failed");
        if (kread(base + 0x200, w, sizeof w)) { logln("-- perf-state table 0x200..0x2ff --"); dump_words(base+0x200, w, sizeof w); }
        if (kread(base + 0x1000, w, sizeof w)) { logln("-- power-state gates 0x1000..0x10ff --"); dump_words(base+0x1000, w, sizeof w); }
        return 0;
    }

    logln("usage: pmgrread obj | pmgrread pmgr <kernelVA>");
    return 0;
}
