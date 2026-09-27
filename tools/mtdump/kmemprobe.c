/* kmemprobe -- does this jailbreak hand out the kernel task port, and can we
 * read kernel memory with it?  armv7, the iPad's own jailbroken iOS 8.4.1.
 *
 * This is the gate for everything that needs to watch iOS from the inside --
 * above all tracing what iOS's own GPU driver (IMGSGX543) writes to the SGX,
 * which is how the GPU command stream gets reverse engineered.  It reads only.
 *
 *   kmemprobe                 test task_for_pid(0); if it works, find the
 *                             kernel's Mach-O base (so the KASLR slide against
 *                             our matching 12H321 kernelcache), and dump its
 *                             header
 *   kmemprobe read VA [N]     read N bytes (default 64) of kernel memory at VA
 *
 * task_for_pid(0) returning the kernel task is what TaiG's 8.4.1 patch is
 * expected to allow; if it does not, this says so and we fall back to static
 * RE of the kernelcache.  The read primitive is vm_read_overwrite, which
 * reads kernel VIRTUAL memory -- device registers (PMGR, the SGX) are physical
 * MMIO mapped somewhere in the kernel we do not know yet, so this does not read
 * them; it reads kernel code and data, which is what a command-stream trace
 * needs.
 *
 * RESULT on this iPad's TaiG 8.4.1 (2026-09-28): closed.  Plain (ldid -S, no
 * entitlements) runs but task_for_pid(0) is KERN_FAILURE and host special
 * port 4 is null -- the kernel task is not handed out.  Signing it with a
 * task_for_pid-allow entitlement (ldid -S<plist>) PANICS the kernel and
 * reboots the device -- do not do that.  So there is no cheap live-kernel
 * route here; the GPU clock config comes from static RE of the kernelcache
 * instead.  Overwriting an executable AMFI has already seen can also get it
 * SIGKILL'd ("Killed: 9") from a stale cdhash -- run a fresh path.
 */
#include <stdio.h>
#include <stdlib.h>

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
mach_port_t mach_host_self(void);
kern_return_t host_get_special_port(mach_port_t host, int node, int which, mach_port_t *port);
#define HOST_KERNEL_PORT 4      /* many 8.x jailbreaks park tfp0 here */

/* the static base of our kernelcache's first (kernel) __TEXT segment */
#define KERNEL_STATIC_BASE 0x80001000u

static mach_port_t kt;

static int kread(vm_address_t va, void *buf, vm_size_t n)
{
    vm_size_t got = 0;
    return vm_read_overwrite(kt, va, n, (vm_address_t)buf, &got) == KERN_SUCCESS && got == n;
}

static void hexdump(vm_address_t va, const unsigned char *p, vm_size_t n)
{
    for (vm_size_t i = 0; i < n; i += 16) {
        printf("  %08x:", va + i);
        for (vm_size_t j = i; j < i + 16 && j < n; j++)
            printf(" %02x", p[j]);
        printf("  ");
        for (vm_size_t j = i; j < i + 16 && j < n; j++)
            putchar(p[j] >= 0x20 && p[j] < 0x7f ? p[j] : '.');
        printf("\n");
    }
}

/* Scan up from 0x80000000 for a 32-bit ARM Mach-O executable header: that is
 * the running kernel.  Steps a page at a time; unmapped pages just fail the
 * read and are skipped. */
static vm_address_t find_kernel(void)
{
    for (vm_address_t va = 0x80000000u; va < 0x90000000u; va += 0x1000u) {
        unsigned int h[4];
        if (!kread(va, h, sizeof h))
            continue;
        if (h[0] == 0xfeedfaceu && h[1] == 12 /* CPU_TYPE_ARM */ && h[3] == 2 /* MH_EXECUTE */)
            return va;
    }
    return 0;
}

int main(int argc, char **argv)
{
    kern_return_t kr = task_for_pid(mach_task_self(), 0, &kt);
    if (kr == KERN_SUCCESS && kt) {
        printf("task_for_pid(0): ok, kernel task port %u\n", kt);
    } else {
        printf("task_for_pid(0): no (%s)\n", mach_error_string(kr));
        /* the entitlement-free route: the kernel task as a host special port */
        mach_port_t hp = mach_host_self();
        kr = host_get_special_port(hp, 0, HOST_KERNEL_PORT, &kt);
        if (kr == KERN_SUCCESS && kt && kt != mach_task_self()) {
            printf("host special port 4: ok, kernel task port %u\n", kt);
        } else {
            printf("host special port 4: no (%s, port %u)\n", mach_error_string(kr), kt);
            printf("this jailbreak does not hand out the kernel task; falling back to static RE.\n");
            return 2;
        }
    }

    if (argc >= 3 && argv[1][0] == 'r') {          /* "read VA [N]" */
        vm_address_t va = (vm_address_t)strtoul(argv[2], 0, 16);
        vm_size_t n = argc > 3 ? (vm_size_t)strtoul(argv[3], 0, 0) : 64;
        static unsigned char buf[4096];
        if (n > sizeof buf)
            n = sizeof buf;
        if (!kread(va, buf, n)) {
            printf("read %08x: unreadable\n", va);
            return 1;
        }
        hexdump(va, buf, n);
        return 0;
    }

    vm_address_t base = find_kernel();
    if (!base) {
        printf("kernel task works, but no Mach-O header found 0x80000000..0x90000000\n");
        return 1;
    }
    printf("kernel Mach-O base: 0x%08x  (KASLR slide 0x%08x vs static 0x%08x)\n",
           base, base - KERNEL_STATIC_BASE, KERNEL_STATIC_BASE);
    unsigned char hdr[64];
    if (kread(base, hdr, sizeof hdr))
        hexdump(base, hdr, sizeof hdr);
    printf("kernel memory is readable -- iOS can be watched from the inside.\n");
    return 0;
}
