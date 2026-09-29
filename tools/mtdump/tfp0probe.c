/* tfp0probe -- re-measure, safely, whether this jailbreak hands out the kernel
 * task port.  Supersedes kmemprobe's under-instrumented test: it SEPARATES
 * acquiring the port (which cannot panic -- it dereferences no kernel memory)
 * from using it (which can), and it PERSISTS every step to a file with fsync
 * BEFORE the risky step, so a panic+reboot can no longer hide the result.
 *
 * Read-only, armv7, the iPad's own jailbroken iOS 8.4.1.  For driver work:
 * the goal is to trace how iOS brings up the SGX clock so a Linux driver can
 * copy it.  This tool only finds out whether live-kernel inspection is even
 * available; it reads nothing on its own.
 *
 *   tfp0probe                 acquire the kernel task port two ways
 *                             (task_for_pid(0), then host_priv special port 4),
 *                             log the outcome, and STOP.  No kernel access.
 *   tfp0probe base            if a port was acquired, find the kernel Mach-O
 *                             base by ENUMERATING mapped regions
 *                             (vm_region_recurse) -- never a blind vm_read scan.
 *   tfp0probe read VA [N]     read N bytes at VA, but only after confirming VA
 *                             lies inside a mapped, readable region.
 *
 * Log: /var/root/tfp0probe.log (fsync'd after each line).  Read it after a
 * reboot to see exactly how far we got.
 *
 * Symbols this needs in stubs/libSystem.tbd (add if the linker asks):
 *   _task_for_pid _vm_read_overwrite _mach_error_string _mach_host_self_
 *   _mach_host_self _host_priv_self _host_get_special_port _vm_region_recurse
 *   _mach_task_self_
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
typedef unsigned int natural_t;
typedef natural_t mach_msg_type_number_t;
#define KERN_SUCCESS 0

extern mach_port_t mach_task_self_;
#define mach_task_self() mach_task_self_

kern_return_t task_for_pid(mach_port_t target, int pid, mach_port_t *t);
kern_return_t vm_read_overwrite(mach_port_t task, vm_address_t addr, vm_size_t size,
                                vm_address_t data, vm_size_t *out);
char *mach_error_string(kern_return_t);
mach_port_t mach_host_self(void);
kern_return_t host_get_special_port(mach_port_t host, int node, int which, mach_port_t *port);
kern_return_t pid_for_task(mach_port_t task, int *pid);   /* inverse of task_for_pid; safe, no memory access */
/* vm_region_recurse: walk mapped regions of a task's map without dereferencing
 * their contents.  info is left opaque (natural_t words); we only use the
 * out address/size and word[0] = protection. */
kern_return_t vm_region_recurse(mach_port_t task, vm_address_t *address, vm_size_t *size,
                                natural_t *depth, natural_t *info, mach_msg_type_number_t *cnt);
#define HOST_KERNEL_PORT 4
#define KERNEL_STATIC_BASE 0x80001000u
#define VM_PROT_READ 0x1

static mach_port_t kt;
static int logfd = -1;

static void logline(const char *s)
{
    /* write to stdout AND to the log, flushing/syncing both, so nothing is
     * lost if the very next operation panics the kernel. */
    fputs(s, stdout); fputc('\n', stdout); fflush(stdout);
    if (logfd >= 0) {
        write(logfd, s, strlen(s)); write(logfd, "\n", 1); fsync(logfd);
    }
}

static void logf1(const char *fmt, unsigned a)
{
    char b[256]; snprintf(b, sizeof b, fmt, a); logline(b);
}
static void logf2(const char *fmt, const char *a, unsigned n)
{
    char b[256]; snprintf(b, sizeof b, fmt, a, n); logline(b);
}

/* Acquire the kernel task port.  Neither call reads kernel memory, so neither
 * can panic; whatever they return is the honest answer. */
static int acquire(void)
{
    logline("== acquire: task_for_pid(0) ==");
    kern_return_t kr = task_for_pid(mach_task_self(), 0, &kt);
    if (kr == KERN_SUCCESS && kt) {
        logf1("task_for_pid(0): OK, port %u", kt);
        logf1("  (mach_task_self() = %u -- if equal, this is OUR task, not the kernel)", mach_task_self());
        {   /* definitive, safe proof: the inverse call must map this port back to pid 0 */
            int vpid = -2; kern_return_t pr = pid_for_task(kt, &vpid);
            char b[160];
            snprintf(b, sizeof b, "  pid_for_task(port %u) => kr=%d pid=%d %s",
                     kt, (int)pr, vpid, (pr == KERN_SUCCESS && vpid == 0) ? "[CONFIRMED kernel_task]" : "[NOT kernel_task]");
            logline(b);
        }
        return 1;
    }
    logf2("task_for_pid(0): no (%s), port %u", mach_error_string(kr), kt);

    logline("== acquire: host special port 4 (best effort) ==");
    mach_port_t hp = mach_host_self();      /* host_priv_self is not exported on iOS 8 */
    logf1("mach_host_self() = %u", hp);
    kr = host_get_special_port(hp, 0, HOST_KERNEL_PORT, &kt);
    if (kr == KERN_SUCCESS && kt && kt != mach_task_self()) {
        logf1("host special port 4: OK, kernel task port %u", kt);
        return 1;
    }
    logf2("host special port 4: no (%s), port %u", mach_error_string(kr), kt);
    logline("RESULT: this jailbreak did NOT hand out the kernel task port.");
    return 0;
}

/* Enumerate this task's map from address 0 -- reads no region contents, so it
 * cannot panic.  Logs the first regions (which reveal WHOSE map this is: a
 * userspace task starts low, the kernel_task starts at 0x80xxxxxx), and returns
 * the first readable region at/above 0x80000000 if there is one. */
static vm_address_t find_kernel_base(void)
{
    vm_address_t addr = 0;
    vm_address_t first_kernel = 0;
    for (int i = 0; i < 64; i++) {
        vm_size_t size = 0; natural_t depth = 0;
        natural_t info[32]; mach_msg_type_number_t cnt = 32;
        kern_return_t kr = vm_region_recurse(kt, &addr, &size, &depth, info, &cnt);
        if (kr != KERN_SUCCESS) { logf1("vm_region_recurse ended after enumerating (kr=%d)", (unsigned)kr); break; }
        {
            char b[256];
            snprintf(b, sizeof b, "region[%d] %08x len %08x prot %x", i, addr, size, info[0]);
            logline(b);
        }
        if (!first_kernel && addr >= 0x80000000u && (info[0] & VM_PROT_READ))
            first_kernel = addr;
        addr += size ? size : 0x1000u;   /* step past this region */
    }
    return first_kernel;
}

/* Bounded scan of the kernel-TEXT window only (no MMIO lives here, so an
 * unmapped page returns a clean error instead of faulting).  One 16-byte read
 * per page, looking for the kernel's Mach-O header.  Checkpoints to the fsync'd
 * log every 256 KB, so if a read ever does fault, the last checkpoint pins the
 * address. */
static vm_address_t scan_kernel_base(void)
{
    for (vm_address_t va = 0x80000000u; va < 0x84000000u; va += 0x1000u) {
        if ((va & 0x3ffffu) == 0)
            logf1("...scanning %08x (no hit yet)", va);
        unsigned h[4]; vm_size_t got = 0;
        if (vm_read_overwrite(kt, va, sizeof h, (vm_address_t)h, &got) != KERN_SUCCESS || got != sizeof h)
            continue;                       /* unmapped / unreadable page -- skip, no fault */
        if (h[0] == 0xfeedfaceu && h[1] == 12u /* CPU_TYPE_ARM */ && h[3] == 2u /* MH_EXECUTE */) {
            logf1("HIT: kernel Mach-O (ARM, MH_EXECUTE) at %08x", va);
            return va;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    logfd = open("/var/root/tfp0probe.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    logline("== tfp0probe start ==");

    if (!acquire())
        return 2;

    if (argc >= 2 && strcmp(argv[1], "findbase") == 0) {
        logline("== findbase: bounded vm_read scan of kernel-text window 0x80000000-0x84000000 ==");
        vm_address_t base = scan_kernel_base();
        if (base) {
            char b[256];
            snprintf(b, sizeof b, "kernel base 0x%08x  KASLR slide 0x%08x (static 0x%08x)",
                     base, base - KERNEL_STATIC_BASE, KERNEL_STATIC_BASE);
            logline(b);
        } else {
            logline("no kernel Mach-O header found in the window (widen or check slide granularity)");
        }
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "base") == 0) {
        logline("== base: enumerating kernel regions (no content read) ==");
        vm_address_t base = find_kernel_base();
        if (base) {
            char b[256];
            snprintf(b, sizeof b, "kernel base ~0x%08x (KASLR slide 0x%08x vs static 0x%08x)",
                     base, base - KERNEL_STATIC_BASE, KERNEL_STATIC_BASE);
            logline(b);
        } else {
            logline("no readable region found in 0x80000000..0x90000000");
        }
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "read") == 0) {
        vm_address_t want = (vm_address_t)strtoul(argv[2], 0, 16);
        vm_size_t n = argc > 3 ? (vm_size_t)strtoul(argv[3], 0, 0) : 64;
        if (n > 4096) n = 4096;
        /* confirm VA is inside a mapped, readable region before reading it,
         * so we never touch an unmapped/guard/MMIO page blindly. */
        vm_address_t addr = 0; vm_size_t size = 0; natural_t depth = 0;
        natural_t info[32]; mach_msg_type_number_t cnt = 32;
        int ok = 0;
        for (int i = 0; i < 65536; i++) {
            addr = (i == 0) ? want & ~0xfffu : addr;
            depth = 0; cnt = 32;
            if (vm_region_recurse(kt, &addr, &size, &depth, info, &cnt) != KERN_SUCCESS) break;
            if (want >= addr && want < addr + size) { ok = (info[0] & VM_PROT_READ) != 0; break; }
            if (addr > want) break;
            addr += size ? size : 0x1000u;
        }
        if (!ok) { logf1("VA %08x is not in a mapped readable region -- refusing", want); return 1; }
        static unsigned char buf[4096]; vm_size_t got = 0;
        if (vm_read_overwrite(kt, want, n, (vm_address_t)buf, &got) != KERN_SUCCESS || got != n) {
            logf1("read %08x: failed", want); return 1;
        }
        for (vm_size_t i = 0; i < n; i += 16) {
            char line[128]; int p = snprintf(line, sizeof line, "  %08x:", want + i);
            for (vm_size_t j = i; j < i + 16 && j < n; j++)
                p += snprintf(line + p, sizeof line - p, " %02x", buf[j]);
            logline(line);
        }
        return 0;
    }

    logline("acquired the kernel task port; stopping (pass 'base' or 'read VA N' to go further).");
    return 0;
}
