// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/irqchip.h>
#include <asm/mach/arch.h>
#include <linux/io.h>

#define APPLE5_FB_BASE  0x9f6fc000
#define APPLE5_FB_SIZE  (1024 * 50)

static void __init apple5_init_early(void)
{
    volatile u32 *fb = (volatile u32 *)APPLE5_FB_BASE;
    int i;
    for (i = 0; i < APPLE5_FB_SIZE; i++)
        fb[i] = 0x00FF0000;
}

static const char *const apple5_dt_compat[] __initconst = {
    "apple,s5l8942x",
    NULL,
};

DT_MACHINE_START(APPLE_S5L8942X, "Apple S5L8942X (iPad mini 1 / p105ap)")
    .dt_compat   = apple5_dt_compat,
    .init_early  = apple5_init_early,
MACHINE_END
