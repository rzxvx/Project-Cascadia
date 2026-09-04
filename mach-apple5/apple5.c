// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irqchip.h>
#include <linux/sizes.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>

void apple_s5l_pmccntr_init(void);

static void __init apple_s5l_map_io(void) {}

static void __init apple_s5l_init_early(void) {}

static void __init apple_s5l_init_irq(void)
{
    irqchip_init();
}

static void __init apple_s5l_init_time(void)
{
    apple_s5l_pmccntr_init();
}

static const char * const apple_s5l_dt_compat[] __initconst = {
    "apple,s5l8940x",
    "apple,s5l8942x",
    NULL
};

DT_MACHINE_START(APPLE_S5L, "Apple S5L (Device Tree)")
    .dt_compat   = apple_s5l_dt_compat,
    .map_io      = apple_s5l_map_io,
    .init_early  = apple_s5l_init_early,
    .init_irq    = apple_s5l_init_irq,
    .init_time   = apple_s5l_init_time,
    .l2c_aux_val = 0,
    .l2c_aux_mask = 0,
MACHINE_END
