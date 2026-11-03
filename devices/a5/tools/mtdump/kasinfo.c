/* kasinfo -- ask the kernel for its text slide via the kas_info debug syscall
 * (root, no tfp0, read only). daibutsu sets PE_i_can_has_debugger, which usually
 * ungates this. If it works, the slide lets us translate kernel addresses safely
 * for the HAL register dump (no blind scanning). armv7 iOS 8.4.1.
 *
 * KAS_INFO_KERNEL_TEXT_SLIDE_SELECTOR = 0 -> returns a uint64 slide.
 * Tries the libc wrapper, then the raw syscall (SYS_kas_info = 439).
 */
#include <stdio.h>
#include <string.h>

typedef unsigned long long u64;
extern int kas_info(int selector, void *value, unsigned long *size);
extern int syscall(int number, ...);
extern int *__error(void);
#define errno (*__error())

int main(void)
{
    u64 slide = 0; unsigned long sz = sizeof slide;
    int r = kas_info(0, &slide, &sz);
    printf("kas_info(0) wrapper: r=%d errno=%d sz=%lu slide=0x%llx\n", r, errno, sz, slide);
    if (r == 0 && slide) {
        printf("KERNEL TEXT SLIDE = 0x%llx  -> base ~ 0x%llx (static 0x80001000)\n",
               slide, 0x80001000ULL + slide);
        return 0;
    }
    slide = 0; sz = sizeof slide;
    r = syscall(439, 0, &slide, &sz);
    printf("kas_info(0) syscall439: r=%d errno=%d sz=%lu slide=0x%llx\n", r, errno, sz, slide);
    if (r == 0 && slide)
        printf("KERNEL TEXT SLIDE = 0x%llx  -> base ~ 0x%llx\n", slide, 0x80001000ULL + slide);
    else
        printf("kas_info unavailable here.\n");
    return 0;
}
