/*
 * timer-bringup v13 — PMGR local enable bits (0x180).
 *
 * v12 proved IBEC_CLKTBL is not a clock bank table (b0=GPIO).
 * PMGR clock regs for CPU IDs already hold gate-like values, but
 * iBEC helper @ 0x2f54 sets bits 0x180 for "enable" and those
 * were CLEAR on p200/p202/p301.
 *
 * This build: for CPU clock IDs only, force PMGR base 0x3F100000,
 * OR 0x180 into the indexed reg. Never call iBEC clock_set.
 * Never touch CLCD switch gates. Paint before/after + GT/PT.
 */
typedef unsigned int u32;

#define WDT_BASE    0x0F103020u
#define PMGR_BASE   0x3F100000u
#define GATE_SWITCH 0x3F101008u
#define IBEC_CLKTBL 0x9FF42160u
#define CLK_EN_BITS 0x180u
#define FB_W 768
#define FB_H 1024
#define BG   0xFF102030u
#define FG   0xFFFFFFFFu
#define ACC  0xFF00FF88u
#define WARN 0xFFFF8800u
#define DIM  0xFF808090u

#include "fb_text8.h"
#define draw_u32(x, y, v, c) draw_u32_hex8(x, y, v, c, 2)

static const u32 cpu_clk_ids[] = {
	0x200u, 0x202u, 0x301u, 0x304u, 0x305u, 0xc00u, 0xc01u, 0xc02u,
};

static void wdt_kill(void) {
	volatile u32 *w = (volatile u32 *)WDT_BASE;
	w[0] = w[1] = w[2] = w[3] = 0;
}
static void fb_fill(int x, int y, int w, int h, u32 c) { fb_text_fill(x, y, w, h, c); }
static void v7_flush(void) {
	__asm__ volatile("dsb");
	__asm__ volatile("isb");
}
static u32 read_cbar(void) {
	u32 v;
	__asm__ volatile("mrc p15, 4, %0, c15, c0, 0" : "=r"(v));
	return v & 0xFFFFE000u;
}
static void busy(u32 n) {
	u32 i;
	for (i = 0; i < n; i++)
		__asm__ volatile("nop");
}
static u32 rd(u32 a) { return *(volatile u32 *)a; }
static void wr(u32 a, u32 v) {
	*(volatile u32 *)a = v;
	v7_flush();
}

static u32 icache_enable(void) {
	u32 sctlr;
	__asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
	sctlr |= (1u << 12);
	__asm__ volatile("mcr p15, 0, %0, c1, c0, 0" :: "r"(sctlr));
	v7_flush();
	__asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(sctlr));
	return sctlr;
}

/* iBEC index math, forced PMGR base. Reject bank!=0 and out-of-window. */
static u32 pmgr_clk_addr(u32 id) {
	u32 bank = id >> 24;
	u32 idx, off, addr;
	if (bank != 0u)
		return 0;
	idx = ((id >> 5) & 0x07fffff8u) | (id & 7u);
	off = (idx << 2) & 0x3fcu;
	addr = PMGR_BASE + off;
	if (addr < PMGR_BASE || addr > (PMGR_BASE + 0x3fcu))
		return 0;
	return addr;
}

/* Same as iBEC 0x2f54 with r1>0: clear then set 0x180. */
static u32 pmgr_or_enable180(void) {
	u32 i, n = 0, addr, v;
	for (i = 0; i < 8u; i++) {
		addr = pmgr_clk_addr(cpu_clk_ids[i]);
		if (!addr)
			continue;
		v = rd(addr);
		v = (v & ~CLK_EN_BITS) | CLK_EN_BITS;
		wr(addr, v);
		n++;
	}
	return n;
}

static void pmu_enable(void) {
	u32 v;
	__asm__ volatile("mcr p15, 0, %0, c9, c14, 0" :: "r"(1u | (1u << 2)));
	__asm__ volatile("mcr p15, 0, %0, c9, c12, 2" :: "r"(0x80000000u));
	v = (1u << 0) | (1u << 1) | (1u << 2);
	__asm__ volatile("mcr p15, 0, %0, c9, c12, 0" :: "r"(v));
	__asm__ volatile("mcr p15, 0, %0, c9, c12, 1" :: "r"(0x80000000u));
	v = 1u;
	__asm__ volatile("mcr p15, 0, %0, c9, c12, 0" :: "r"(v));
	v7_flush();
}

static u32 pmcc_delta(void) {
	u32 c0, c1;
	__asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(c0));
	busy(2000000u);
	__asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(c1));
	return c1 - c0;
}

static u32 gt_probe(u32 cbar, u32 *ctl) {
	volatile u32 *gt = (volatile u32 *)(cbar + 0x200);
	u32 c0, c1;
	gt[0] = 0;
	gt[1] = 0;
	gt[2] = 1u;
	v7_flush();
	*ctl = gt[2];
	c0 = gt[0];
	busy(2000000u);
	c1 = gt[0];
	return c1 - c0;
}

static u32 pt_probe(u32 cbar, u32 *ctl) {
	volatile u32 *pt = (volatile u32 *)(cbar + 0x600);
	u32 c0, c1;
	pt[0] = 0xffffffffu;
	pt[2] = 0;
	pt[2] = 1u;
	v7_flush();
	*ctl = pt[2];
	c0 = pt[1];
	busy(2000000u);
	c1 = pt[1];
	return c0 - c1;
}

int main(void) {
	u32 cbar, sctlr, nset, pmcc, gt_ctl, gt_d, pt_ctl, pt_d;
	u32 before, after, g68, b0;

	wdt_kill();
	fb_fill(0, 0, FB_W, FB_H, BG);
	cbar = read_cbar();

	b0 = rd(IBEC_CLKTBL);
	before = rd(pmgr_clk_addr(0x200u));
	g68 = rd(GATE_SWITCH + (68u << 2));

	draw_u32(8, 20, b0, WARN);
	draw_u32(8, 40, before, FG);
	draw_u32(8, 60, g68, ACC);
	fb_fill(0, 0, FB_W, 10, DIM);
	v7_flush();

	sctlr = icache_enable();
	nset = pmgr_or_enable180();
	after = rd(pmgr_clk_addr(0x200u));

	pmu_enable();
	pmcc = pmcc_delta();
	gt_d = gt_probe(cbar, &gt_ctl);
	pt_d = pt_probe(cbar, &pt_ctl);

	draw_u32(8, 80, sctlr, (sctlr & (1u << 12)) ? ACC : WARN);
	draw_u32(8, 100, nset, (nset == 8u) ? ACC : WARN);
	draw_u32(8, 120, after, (after & CLK_EN_BITS) == CLK_EN_BITS ? ACC : WARN);
	draw_u32(8, 140, pmcc, pmcc ? ACC : WARN);
	draw_u32(8, 160, gt_ctl, gt_ctl ? ACC : WARN);
	draw_u32(8, 180, gt_d, gt_d ? ACC : WARN);
	draw_u32(8, 200, pt_ctl, pt_ctl ? ACC : WARN);
	draw_u32(8, 220, pt_d, pt_d ? ACC : WARN);
	draw_u32(8, 240, cbar, DIM);
	draw_u32(8, 260, pmgr_clk_addr(0x200u), DIM);

	fb_fill(0, 0, FB_W, 10, (gt_d || pt_d) ? ACC : (pmcc ? DIM : WARN));
	v7_flush();
	for (;;)
		__asm__ volatile("wfi");
	return 0;
}
