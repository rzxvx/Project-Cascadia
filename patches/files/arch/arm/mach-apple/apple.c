// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple 32-bit S5L SoC support (S5L8940X / S5L8942X, "A5").
 */

#include <linux/init.h>
#include <linux/io.h>
#include <linux/irqchip.h>
#include <linux/sizes.h>
#include <linux/irq.h>
#include <linux/bits.h>
#include <linux/memblock.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <asm/barrier.h>
#include <asm/thread_info.h>
#include <asm/exception.h>
#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/memory.h>

#include "p105_fb_dbg.h"

extern bool early_boot_irqs_disabled;
extern pid_t user_mode_thread(int (*fn)(void *), void *arg, unsigned long flags);
void apple_aic1_quiesce(void);
void apple_aic1_enable(void);
void apple_aic1_arm_cpu0(void);
void apple_aic1_release_escape(void);
void apple_aic1_hw_quiesce(void);
void apple_aic1_hw_quiesce_quiet(void);

/* Bump when changing rest_init spawn path so FB confirms the flashed image. */
#define APPLE_BOOT_STAMP	0x26082634ul

/*
 * Lab IRQ stub forced SPSR.I on return against sticky nIRQ.  Keep true through
 * first enables; apple_aic1_release_escape() clears it after 0x5004 drain +
 * successful irq_thaw (aic1-lab v30: I0 HOLD OK without force).
 */
bool apple_aic1_early_irq_escape = true;

void __init apple_s5l_pmccntr_init(void);

/*
 * P105AP Recovery live scanout (confirmed): phys 0x9F6FC000, portrait
 * 768x1024, stride 3072 (=768*4), BGRA.
 */
#define P105_FB_PHYS	0x9f6fc000
#define P105_FB_STRIDE	3072

unsigned long p105_fb_phys = P105_FB_PHYS;
unsigned long p105_fb_stride = P105_FB_STRIDE;
unsigned p105_vis_row0 = 4;

#define P105_AIC_PHYS	0x3f200000
#define P105_AIC_VIRT	0xF0500000UL

/* aic1-lab v30: EVENT is per-CPU @ 0x5004; 0x2004 stays idle on aic,1 */
#define P105_AIC_CONFIG		0x0010
#define P105_AIC_EVENT		0x2004
#define P105_AIC_EVENT_CPU0	0x5004
#define P105_AIC_IPI_ACK	0x200c
#define P105_AIC_IPI_MASK_SET	0x2024
#define P105_AIC_SW_CLR		0x4080
#define P105_AIC_MASK_SET	0x4100
#define P105_AIC_TARGET_CPU	0x3000
#define P105_AIC_NR_IRQ		192
#define P105_AIC_NR_WORDS	6

static struct map_desc apple_aic_desc __initdata = {
	.virtual	= P105_AIC_VIRT,
	.pfn		= __phys_to_pfn(P105_AIC_PHYS),
	.length		= SZ_1M,
	.type		= MT_DEVICE,
};

/*
 * Lab: CBAR phys 0x3E100000 R/W is fine bare-metal.  Under Linux, both
 * ioremap and static MT_DEVICE map of that window hang (aic1q33 stuck after
 * printing CP15 CBAR).  Skip GIC MMIO; rely on AIC EVENT@0x5004 drain.
 */
static bool apple_aic_quiesce_silent;

void apple_a9_periph_quiesce(void)
{
	u32 cbar;

	asm volatile("mrc p15, 4, %0, c15, c0, 0" : "=r"(cbar));
	if (!apple_aic_quiesce_silent) {
		p105_fb_dbg_hex("cbar", cbar & 0xffffe000u);
		p105_fb_dbg("cbar_skip");
	}
}

void apple_a9_gic_drain(void)
{
	/* no CBAR map — AIC drain only */
}

/*
 * Early aic,1 quiesce (aic1-lab v30 P0 + E5 drain):
 * TARGET=0, MASK, IPI, drain EVENT@0x5004 (and 0x2004), CFG.enable=0.
 * Never PMGR gate 0x4D while AIC must live.
 */
void apple_aic1_hw_quiesce(void)
{
	void __iomem *base = (void __iomem *)P105_AIC_VIRT;
	unsigned int i, n, cpu;
	u32 cfg;

	apple_a9_periph_quiesce();

	for (i = 0; i < P105_AIC_NR_WORDS; i++) {
		writel_relaxed(~0U, base + P105_AIC_MASK_SET + i * 4);
		writel_relaxed(~0U, base + P105_AIC_SW_CLR + i * 4);
	}

	for (i = 0; i < P105_AIC_NR_IRQ; i++)
		writel_relaxed(0, base + P105_AIC_TARGET_CPU + i * 4);

	writel_relaxed(~0U, base + P105_AIC_IPI_MASK_SET);
	writel_relaxed(BIT(31) | BIT(0), base + P105_AIC_IPI_ACK);
	writel_relaxed(~0U, base + P105_AIC_IPI_MASK_SET);
	for (i = 0; i < 2; i++) {
		writel_relaxed(BIT(31) | BIT(0), base + 0x500c + (i << 7));
		writel_relaxed(~0U, base + 0x5024 + (i << 7));
	}

	for (n = 0; n < 512; n++) {
		u32 any = readl_relaxed(base + P105_AIC_EVENT);

		for (cpu = 0; cpu < 2; cpu++)
			any |= readl_relaxed(base + P105_AIC_EVENT_CPU0 +
					     (cpu << 7));
		if (!any)
			break;
	}

	for (i = 0; i < P105_AIC_NR_WORDS; i++)
		writel_relaxed(~0U, base + P105_AIC_MASK_SET + i * 4);

	cfg = readl_relaxed(base + P105_AIC_CONFIG);
	cfg |= 0xE0000000U;
	cfg &= ~BIT(0);
	writel_relaxed(cfg, base + P105_AIC_CONFIG);
	dsb(sy);

	if (!apple_aic_quiesce_silent) {
		p105_fb_dbg_hex("cfg", readl_relaxed(base + P105_AIC_CONFIG));
		p105_fb_dbg_hex("e5", readl_relaxed(base + P105_AIC_EVENT_CPU0));
		p105_fb_dbg_hex("tgt", readl_relaxed(base + P105_AIC_TARGET_CPU));
	}
}

/** Silent P0 quiesce for irq_thaw (no FB dump flood). */
void apple_aic1_hw_quiesce_quiet(void)
{
	apple_aic_quiesce_silent = true;
	apple_aic1_hw_quiesce();
	apple_aic_quiesce_silent = false;
}

/* Affinity to CPU0 after IRQs are on; drain per-CPU EVENT. */
void apple_aic1_arm_cpu0(void)
{
	void __iomem *base = (void __iomem *)P105_AIC_VIRT;
	unsigned int i, n, cpu;

	for (i = 0; i < P105_AIC_NR_WORDS; i++) {
		writel_relaxed(~0U, base + P105_AIC_MASK_SET + i * 4);
		writel_relaxed(~0U, base + P105_AIC_SW_CLR + i * 4);
	}
	for (i = 0; i < P105_AIC_NR_IRQ; i++)
		writel_relaxed(BIT(0), base + P105_AIC_TARGET_CPU + i * 4);
	for (i = 0; i < P105_AIC_NR_WORDS; i++)
		writel_relaxed(~0U, base + P105_AIC_MASK_SET + i * 4);
	for (n = 0; n < 512; n++) {
		u32 any = readl_relaxed(base + P105_AIC_EVENT);

		for (cpu = 0; cpu < 2; cpu++)
			any |= readl_relaxed(base + P105_AIC_EVENT_CPU0 +
					     (cpu << 7));
		if (!any)
			break;
	}
}

struct apple_spawn_args {
	int (*fn)(void *);
	void *arg;
	unsigned long flags;
	pid_t pid;
	unsigned long old_sp;
};

static void __noclone noinline apple_spawn_fn(void *a)
{
	struct apple_spawn_args *s = a;

	s->pid = user_mode_thread(s->fn, s->arg, s->flags);
}

static noinline pid_t apple_spawn_on_alt_stack(int (*fn)(void *), void *arg,
					       unsigned long flags)
{
	struct apple_spawn_args s = {
		.fn = fn,
		.arg = arg,
		.flags = flags,
		.pid = -ENOMEM,
	};
	unsigned long sp, old_sp;
	u8 *stk;
	void (*spawn)(void *) = apple_spawn_fn;

	stk = memblock_alloc(THREAD_SIZE, THREAD_SIZE);
	if (!stk)
		return -ENOMEM;

	sp = (unsigned long)stk + THREAD_SIZE - 8;
	sp &= ~7UL;

	asm volatile(
		"mov	%0, sp\n"
		"mov	sp, %1\n"
		"mov	r0, %2\n"
		"blx	%3\n"
		"mov	sp, %0\n"
		: "=&r" (old_sp)
		: "r" (sp), "r" (&s), "r" (spawn)
		: "r0", "lr", "memory");

	s.old_sp = old_sp;
	return s.pid;
}

static unsigned long apple_current_sp(void)
{
	unsigned long sp;

	asm volatile("mov %0, sp" : "=r" (sp));
	return sp;
}

pid_t __init apple_s5l_rest_spawn_init(int (*fn)(void *), void *arg,
				       unsigned long flags)
{
	unsigned long top = (unsigned long)current->stack + THREAD_SIZE;
	pid_t pid;

	p105_fb_dbg_hex("bld", APPLE_BOOT_STAMP);
	p105_fb_dbg_hex("sp", apple_current_sp());
	p105_fb_dbg_hex("stk", top - apple_current_sp());

	/*
	 * IRQs already probed+frozen in start_kernel (irq_frz).  Do not
	 * re-quiesce/enable here.  Spawn on the normal task stack — alt-stack
	 * SP breaks on_accessible_stack()/canary checks on the way out of
	 * kernel_clone (hang after kc_woke).
	 */
	p105_fb_dbg("irq_ok");

	p105_fb_dbg("umt_go");
	flags |= CLONE_FILES;
	pid = user_mode_thread(fn, arg, flags);
	p105_fb_dbg("umt_ret");
	p105_fb_dbg_hex("pid", (unsigned long)pid);

	return pid;
}

static phys_addr_t p105_fb_section(void)
{
	phys_addr_t phys = (phys_addr_t)p105_fb_phys & ~(SZ_1M - 1);

	if (phys < 0x9f000000 || phys >= 0x9ff00000)
		phys = P105_FB_PHYS & ~(SZ_1M - 1);
	return phys;
}

static void __init apple_s5l_reserve(void)
{
	phys_addr_t phys = p105_fb_section();

	memblock_remove(phys, 0x9f6fc000 - phys);
}

static void __init apple_s5l_map_io(void)
{
	/*
	 * Do not iotable_init the framebuffer. Mapping 0x9f6fxxxx as
	 * MT_DEVICE_WC at 0xF0000000 hung at mio_fb (create_mapping /
	 * memblock for the static VM). Early identity map of
	 * 0x9f000000–0xa0000000 (kept in prepare_page_table) is enough
	 * for p105_fb_dbg; simplefb can ioremap later.
	 */
	p105_fb_dbg("mio_enter");
	p105_fb_dbg("aic1q34");	/* EVENT@0x5004; CBAR map skipped; release escape */
	p105_fb_phys = P105_FB_PHYS;
	p105_fb_stride = P105_FB_STRIDE;

	p105_fb_dbg("mio_aic");
	iotable_init(&apple_aic_desc, 1);
	p105_fb_dbg("mio_nocbar");

	p105_vis_row0 = 4;
	/* Stay on phys identity — never p105_fb_dbg_use_virt(). */
	p105_fb_dbg("map_io");
	p105_fb_dbg_hex("fb", p105_fb_phys);
	p105_fb_dbg_hex("str", p105_fb_stride);
}

static void __init apple_s5l_init_early(void)
{
	p105_fb_dbg("init_early");
}

static void __init apple_s5l_init_irq(void)
{
	p105_fb_dbg("irqchip_init");
	irqchip_init();
	p105_fb_dbg("irqchip_done");
}

static void __init apple_s5l_init_time(void)
{
	p105_fb_dbg("pmccntr_init");
	apple_s5l_pmccntr_init();
	p105_fb_dbg("pmccntr_done");
}

static const char * const apple_s5l_dt_compat[] __initconst = {
	"apple,s5l8940x",
	"apple,s5l8942x",
	NULL
};

DT_MACHINE_START(APPLE_S5L, "Apple S5L (Device Tree)")
	.dt_compat	= apple_s5l_dt_compat,
	.reserve	= apple_s5l_reserve,
	.map_io		= apple_s5l_map_io,
	.init_early	= apple_s5l_init_early,
	.init_irq	= apple_s5l_init_irq,
	.init_time	= apple_s5l_init_time,
	.l2c_aux_val	= 0,
	.l2c_aux_mask	= 0,
MACHINE_END
