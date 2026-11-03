/* leakprobe -- does sysctl KERN_PROC hand userspace a real (unpermuted) kernel
 * pointer? Safe read-only probe (no tfp0, no kernel access, no reboot risk).
 * If kinfo_proc contains aligned kernel-range pointers (e_paddr = this proc's
 * kernel address, p_vmspace, p_sigacts, ...), that is the foothold to derive the
 * VM_KERNEL_ADDRPERM constant for the HAL register read. armv7 iOS 8.4.1.
 */
#include <stdio.h>
#include <string.h>

int sysctl(int *name, unsigned namelen, void *old, unsigned long *oldlen, void *newp, unsigned long newlen);
int getpid(void);

#define CTL_KERN 1
#define KERN_PROC 14
#define KERN_PROC_PID 1

int main(void)
{
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    static unsigned char buf[1024];
    unsigned long len = sizeof buf;
    if (sysctl(mib, 4, buf, &len, 0, 0) != 0) { printf("sysctl KERN_PROC failed\n"); return 1; }
    printf("kinfo_proc len=%lu  pid=%d\n", len, getpid());

    /* dump as 32-bit words, flagging kernel-range, aligned candidates */
    unsigned int *w = (unsigned int *)buf;
    unsigned long n = len / 4;
    for (unsigned long i = 0; i < n; i += 4) {
        char line[160]; int p = snprintf(line, sizeof line, "  +%03lx:", i*4);
        for (unsigned long j = i; j < i+4 && j < n; j++) {
            unsigned int v = w[j];
            int kern = (v >= 0x80000000u) && ((v & 3) == 0);   /* aligned, kernel range */
            p += snprintf(line+p, sizeof line-p, " %08x%s", v, kern ? "*" : " ");
        }
        printf("%s\n", line);
    }
    printf("(* = aligned value in kernel range 0x80000000+; e_paddr is this proc's kernel address)\n");
    return 0;
}
