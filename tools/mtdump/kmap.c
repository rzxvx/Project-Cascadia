/* kmap -- enumerate the kernel task's VM map via vm_region_recurse. PURE QUERY:
 * it reads vm_map metadata only, never the region contents, so it cannot fault
 * and cannot touch device MMIO -- fully reboot-safe. armv7 iOS 8.4.1.
 *
 * Purpose (Route B, after the KERN_PROC e_paddr leak came back zeroed): the
 * mach_port_kobject path returns a VM_KERNEL_ADDRPERM-permuted address that
 * faults on vm_read. vm_region_recurse instead returns REAL kernel VAs of the
 * map entries (it is not permuted), plus each entry's user_tag (VM_MEMORY_*)
 * and size -- enough to locate the IOKit device mapping that covers PMGR
 * (phys 0x3f100000) without any blind read. Once identified, a targeted read
 * (pmgrread) fetches the enabled GFX clock values.
 *
 * Needs the task_for_pid-allow entitlement (sign with tfp0.entitlements).
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

typedef unsigned int mach_port_t;
typedef int kern_return_t;
typedef unsigned int vm_address_t;
typedef unsigned int vm_size_t;
#define KERN_SUCCESS 0

extern mach_port_t mach_task_self_;
#define mach_task_self() mach_task_self_
kern_return_t task_for_pid(mach_port_t, int, mach_port_t *);
char *mach_error_string(kern_return_t);
kern_return_t vm_region_recurse(mach_port_t task, vm_address_t *address, vm_size_t *size,
                                unsigned int *nesting_depth, int *info, unsigned int *infoCnt);

/* struct vm_region_submap_info (32-bit, non-_64), 19 ints */
struct submap_info {
    unsigned int protection, max_protection, inheritance, offset, user_tag,
                 pages_resident, pages_shared_now_private, pages_swapped_out,
                 pages_dirtied, ref_count;
    unsigned short shadow_depth;
    unsigned char external_pager, share_mode;
    unsigned int is_submap, behavior, object_id;
    unsigned short user_wired_count;
};
#define SUBMAP_INFO_COUNT (sizeof(struct submap_info)/sizeof(int))

static void logln(int fd, const char *s){ fputs(s,stdout); fputc('\n',stdout); fflush(stdout);
    if(fd>=0){ write(fd,s,strlen(s)); write(fd,"\n",1); fsync(fd);} }

int main(void)
{
    int fd = open("/var/root/kmap.log", O_WRONLY|O_CREAT|O_TRUNC, 0644);
    logln(fd, "== kmap: kernel VM map (vm_region_recurse, query-only) ==");

    mach_port_t kt = 0;
    kern_return_t kr = task_for_pid(mach_task_self(), 0, &kt);
    if (kr != KERN_SUCCESS || !kt){ char b[128]; snprintf(b,sizeof b,"task_for_pid(0) failed: %s", mach_error_string(kr)); logln(fd,b); return 2; }
    { char b[64]; snprintf(b,sizeof b,"kernel task port %u", kt); logln(fd,b); }

    vm_address_t addr = 0;
    unsigned int count = 0;
    char b[200];
    logln(fd, "  addr       size       prot mx tag   share submap  depth");
    for (;;) {
        vm_size_t size = 0;
        unsigned int depth = 100;               /* recurse into submaps */
        struct submap_info info;
        unsigned int cnt = SUBMAP_INFO_COUNT;
        kr = vm_region_recurse(kt, &addr, &size, &depth, (int *)&info, &cnt);
        if (kr != KERN_SUCCESS) break;
        snprintf(b, sizeof b, "  %08x  %08x   %x  %x  %-4u  %u     %u       %u",
                 addr, size, info.protection, info.max_protection, info.user_tag,
                 info.share_mode, info.is_submap, depth);
        logln(fd, b);
        if (++count > 4000) { logln(fd, "  ...(capped)"); break; }
        addr += size;                            /* next region */
        if (addr == 0) break;                    /* wrapped */
    }
    snprintf(b, sizeof b, "== %u regions ==", count); logln(fd, b);
    return 0;
}
