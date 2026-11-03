/* K93-only RAM diagnostic. No storage access; inspect only loaded images. */
#include "fb_text8.h"
#include "staging-generated.h"

static void line(int row, const char *s)
{
    draw_string8(8, 40 + row * 24, s, 0xffffffffu, 2);
}

static void value(int row, const char *label, u32 n)
{
    line(row, label);
    draw_u32_hex8(240, 40 + row * 24, n, 0xff00ff88u, 2);
}

u32 main(void)
{
    u32 mpidr, sctlr;
    const volatile u32 *z = (const volatile u32 *)0x80008000u;
    const volatile u32 *dt = (const volatile u32 *)DTB_PHYS;
    fb_text_fill(0, 0, FB_TEXT_W, FB_TEXT_H, 0xff102030u);
    line(0, "K93 RAM diagnostic - 1024/4096 trial");
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
    __asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
    value(1, "MPIDR", mpidr);
    value(2, "SCTLR", sctlr);
    value(3, "ZIMAGE MAGIC", z[0x24 / 4]);
    value(4, "DTB ADDRESS", DTB_PHYS);
    value(5, "DTB MAGIC LE", dt[0]);
    if (z[0x24 / 4] != 0x016f2818u || dt[0] != 0xedfe0dd0u) {
        line(6, "STOP: loaded image header mismatch");
        for (;;) __asm__ volatile("wfi");
    }
    line(6, "Headers OK - entering handoff");
    line(7, "Blocks: 1 L1-in  2 L1-out  3 L2-out  4 MMU-off");
    /* Assembly paints four blocks at y=280 without using the stack. */
    for (int i = 0; i < 4; i++)
        fb_text_fill(8 + i * 32, 280, 16, 16, 0xff505050u);
    return DTB_PHYS;
}
