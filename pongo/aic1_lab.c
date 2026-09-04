/*
 * aic1-lab v68 — no iBSS park. Boot with known-good direct.dfu (ep4 magic).
 * USB: publish LAB: into serial if found, then WFI (ep4 enters via bx — no return).
 */
typedef unsigned int u32;
typedef unsigned char u8;

#define AIC_BASE	0x3F200000u
#define AIC_HWM		0x4200u
#define AIC_EVENT	0x2004u
#define AIC_EVENT_CPU0	0x5004u
#define AIC_IPI_SEND	0x2008u
#define AIC_IPI_ACK	0x200cu
#define AIC_IPI_MASK_SET 0x2024u
#define AIC_IPI_MASK_CLR 0x2028u
#define PMGR_BASE	0x3F100000u
#define PMGR_GATE0	0x1008u
#define PMGR_HWM_GATE	0x4Du
#define PMGR_CPU_REG	0x6000u
#define PMGR_APPLY_1180	0x1180u
#define PMGR_APPLY_1200	0x1200u
#define PMGR_APPLY_1204	0x1204u
#define SCU_CTRL	0x00u
#define SCU_CFG		0x04u
#define SCU_PWR		0x08u
#define SCU_CTRL_ENABLE	0x1u
#define SEC_MAGIC	0xC0DE0001u

#define BG		0xFF102030u
#define FG		0xFFFFFFFFu
#define ACC		0xFF00FF88u
#define WARN		0xFFFF8800u
#define DIM		0xFF808090u
#define SCALE		2
#define LINE		16
#define ROW0		48
#define COL_W		380
#define COL0_X		8
#define STATUS_Y	8
#define STATUS_H	20

#define DELAY_GATE	2500000u
#define DELAY_PHASE	1500000u

#include "fb_text8.h"
#define MAX_ROWS	((FB_TEXT_H - ROW0) / LINE)
#define hx(x, y, v, c) draw_u32_hex8(x, y, v, c, SCALE)

static int g_row;
static int g_col;
volatile u32 g_irq_n;
volatile u32 g_spurious;
volatile u32 g_force_i = 1;
extern volatile u32 g_step;

volatile u32 g_cap_ev;
volatile u32 g_cap_e5;
volatile u32 g_cap_iar;
volatile u32 g_cap_done;

static u32 g_cbar;
static u32 g_nr, g_words;

u32 cpu_probe_msri(void);
void secondary_stub(void);
void secondary_irq_hook(void);
extern volatile u32 secondary_flag;
extern volatile u32 g_saved_irq_target;
extern u32 secondary_park_tramp[];
extern u32 secondary_park_tramp_end[];

static void line(const char *s, u32 c);
static void line_hex(const char *tag, u32 v);
static void line_stat(const char *tag, u32 cpsr);

static void delay(u32 n)
{
	volatile u32 i;

	for (i = 0; i < n; i++)
		;
}

static int cur_x(void) { return COL0_X + g_col * COL_W; }
static int cur_y(void) { return ROW0 + g_row * LINE; }

static void next_line(void)
{
	g_row++;
	if (g_row >= MAX_ROWS) {
		g_row = 0;
		g_col++;
	}
}

static u32 rd(u32 off)
{
	return *(volatile u32 *)(AIC_BASE + off);
}

static void wr(u32 off, u32 v)
{
	*(volatile u32 *)(AIC_BASE + off) = v;
	__asm__ volatile("dsb" ::: "memory");
}

static u32 cbar_rd(u32 off)
{
	return *(volatile u32 *)(g_cbar + off);
}

static void cbar_wr(u32 off, u32 v)
{
	*(volatile u32 *)(g_cbar + off) = v;
	__asm__ volatile("dsb" ::: "memory");
}

static u32 pmgr_rd(u32 off)
{
	return *(volatile u32 *)(PMGR_BASE + off);
}

static void pmgr_wr(u32 off, u32 v)
{
	*(volatile u32 *)(PMGR_BASE + off) = v;
	__asm__ volatile("dsb" ::: "memory");
}

static void status(const char *tag, u32 id)
{
	fb_text_fill(0, STATUS_Y, FB_TEXT_W, STATUS_H, BG);
	draw_string8(8, STATUS_Y, tag, WARN, SCALE);
	hx(8 + 5 * 8 * SCALE, STATUS_Y, id, FG);
	__asm__ volatile("dsb; isb" ::: "memory");
}

static void line(const char *s, u32 c)
{
	draw_string8(cur_x(), cur_y(), s, c, SCALE);
	next_line();
}

static void line_hex(const char *tag, u32 v)
{
	int x = cur_x();
	int y = cur_y();

	draw_string8(x, y, tag, DIM, SCALE);
	hx(x + 5 * 8 * SCALE, y, v, FG);
	next_line();
}

static void line_stat(const char *tag, u32 cpsr)
{
	int x = cur_x();
	int y = cur_y();

	draw_string8(x, y, tag, DIM, SCALE);
	hx(x + 5 * 8 * SCALE, y, cpsr, FG);
	draw_string8(x + 15 * 8 * SCALE, y,
		     ((cpsr >> 7) & 1) ? "I1" : "I0", ACC, SCALE);
	next_line();
}

static void wdt_kill(void)
{
	volatile u32 *w = (volatile u32 *)0x0F103020u;

	w[0] = w[1] = w[2] = w[3] = 0;
}

static u32 read_cbar(void)
{
	u32 v;

	asm volatile("mrc p15, 4, %0, c15, c0, 0" : "=r"(v));
	return v & 0xffffe000u;
}

void aic1_lab_irq(void)
{
	u32 e5, ev, iar = 0;

	g_irq_n++;
	e5 = rd(AIC_EVENT_CPU0);
	ev = rd(AIC_EVENT);
	if (!g_cap_done) {
		g_cap_ev = ev;
		g_cap_e5 = e5;
		g_cap_iar = iar;
		g_cap_done = 1;
	}
	if (e5)
		(void)rd(AIC_EVENT_CPU0);
	if (ev)
		(void)rd(AIC_EVENT);
	wr(AIC_IPI_ACK, 0x80000001u);
}

void aic1_lab_exc(u32 kind, u32 lr, u32 fsr)
{
	(void)kind;
	(void)lr;
	(void)fsr;
	g_spurious++;
}

static void aic_drain(void)
{
	u32 n, cpu;

	for (n = 0; n < 512; n++) {
		u32 any = rd(AIC_EVENT);

		for (cpu = 0; cpu < 2; cpu++)
			any |= rd(AIC_EVENT_CPU0 + (cpu << 7));
		if (!any)
			break;
	}
}

static void aic_mask_all(void)
{
	u32 i;

	for (i = 0; i < g_words; i++) {
		wr(0x4100 + i * 4, ~0u);
		wr(0x4080 + i * 4, ~0u);
	}
}

static void aic_target_all(u32 dest)
{
	u32 t;

	for (t = 0; t < g_nr; t++)
		wr(0x3000 + t * 4, dest);
}

static void aic_ipi_quiesce(void)
{
	u32 i;

	wr(0x2024, ~0u);
	wr(0x200c, 0x80000001u);
	wr(0x2024, ~0u);
	for (i = 0; i < 2; i++) {
		wr(0x500c + (i << 7), 0x80000001u);
		wr(0x5024 + (i << 7), ~0u);
	}
}

static void aic_p0(void)
{
	aic_target_all(0);
	aic_mask_all();
	aic_ipi_quiesce();
	aic_drain();
	wr(0x0010, rd(0x0010) & ~1u);
}

static void gic_kill(void)
{
	u32 iar, n;

	if (!g_cbar)
		return;
	cbar_wr(0x208, 0);
	cbar_wr(0x20c, 1);
	cbar_wr(0x608, 0);
	cbar_wr(0x60c, 1);
	cbar_wr(0x100, 0);
	cbar_wr(0x1000, 0);
	cbar_wr(0x1180, ~0u);
	for (n = 0; n < 32; n++) {
		iar = cbar_rd(0x10c);
		if (iar == 1023u)
			break;
		cbar_wr(0x110, iar);
	}
}

static void aic_arm_sw0(void)
{
	wr(0x0010, (rd(0x0010) | 0xE0000000u) | 1u);
	wr(0x3000, 1u);
	wr(0x4100, 1u);
	wr(0x4180, 1u);
	wr(0x4080, 1u);
	__asm__ volatile("dsb; isb" ::: "memory");
	wr(0x4000, 1u);
}

static int psafe_ok(const char *tag)
{
	u32 n0, cpsr, d;

	g_force_i = 1;
	__asm__ volatile("cpsid i; isb" ::: "memory");
	g_cap_done = 0;
	n0 = g_irq_n;
	cpsr = cpu_probe_msri();
	d = g_irq_n - n0;
	line(tag, DIM);
	line_stat("Ps", cpsr);
	line_hex("dIRQ", d);
	if (g_cap_done) {
		line_hex("cEV", g_cap_ev);
		line_hex("cE5", g_cap_e5);
		line_hex("cIA", g_cap_iar);
	}
	return (((cpsr >> 7) & 1) == 0) && (d == 0);
}

static void report_cap(const char *tag, u32 n0)
{
	line(tag, DIM);
	line_hex("dIRQ", g_irq_n - n0);
	if (g_cap_done) {
		line_hex("cEV", g_cap_ev);
		line_hex("cE5", g_cap_e5);
		if (g_cap_e5 == 0x00010000u)
			line("SW DELIV OK", ACC);
	}
}

#if 0 /* v35: SMP-only glass; keep proven A/B/C for later */
static void phase_sw_deliv(void)
{
	u32 n0, i;

	line("--- SW DELIV ---", ACC);
	status("phA ", 0);
	gic_kill();
	aic_p0();
	line_hex("H0", rd(AIC_HWM + 0));
	line_hex("H3", rd(AIC_HWM + 12));
	line_hex("H4", rd(AIC_HWM + 16));

	wr(0x0010, (rd(0x0010) | 0xE0000000u) | 1u);
	aic_arm_sw0();
	line_hex("SW0", rd(0x4000));

	for (i = 0; i < 3; i++) {
		g_force_i = 1;
		g_cap_done = 0;
		n0 = g_irq_n;
		__asm__ volatile("cpsid i; isb" ::: "memory");
		wr(0x4080, 1u);
		wr(0x4000, 1u);
		__asm__ volatile("cpsie i; isb" ::: "memory");
		delay(200000u);
		__asm__ volatile("cpsid i; isb" ::: "memory");
		report_cap(i == 0 ? "try0" : (i == 1 ? "try1" : "try2"), n0);
		if (i == 0) {
			line_hex("peek", rd(AIC_EVENT_CPU0));
			if (rd(AIC_EVENT_CPU0) == 0 && g_cap_e5 == 0x00010000u)
				line("fmt OK", ACC);
			aic_arm_sw0();
			line_hex("SW1", rd(0x4000));
		}
	}
	line("phA done", DIM);
	delay(DELAY_PHASE);
}

static void phase_ihold(void)
{
	u32 n0, cpsr, i;

	line("--- Ihold ---", ACC);
	status("phB ", 1);
	aic_p0();
	aic_drain();
	wr(0x0010, (rd(0x0010) | 0xE0000000u) | 1u);
	line_hex("H0", rd(AIC_HWM + 0));
	line_hex("E5", rd(AIC_EVENT_CPU0));

	if (!psafe_ok("Psafe")) {
		line("Psafe FAIL", WARN);
		return;
	}
	line("Psafe OK", ACC);

	g_force_i = 0;
	n0 = g_irq_n;
	__asm__ volatile("cpsie i; isb" ::: "memory");
	__asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
	line_stat("P0", cpsr);
	line("hold", DIM);
	for (i = 0; i < 400000u; i++)
		__asm__ volatile("nop");
	__asm__ volatile("mrs %0, cpsr" : "=r"(cpsr));
	line_stat("hold", cpsr);
	line_hex("dIRQ", g_irq_n - n0);
	g_force_i = 1;
	__asm__ volatile("cpsid i; isb" ::: "memory");

	if (((cpsr >> 7) & 1) == 0 && (g_irq_n == n0))
		line("I0 HOLD OK", ACC);
	else
		line("I0 FAIL", WARN);

	line("phB done", DIM);
	delay(DELAY_PHASE);
}

static void phase_pmgr_4d(void)
{
	u32 before, sw_b, sw_a;

	line("--- 4D demo ---", ACC);
	status("phC ", PMGR_HWM_GATE);

	before = pmgr_rd(PMGR_GATE0 + PMGR_HWM_GATE * 4);
	line_hex("g4D", before);
	line_hex("H0b", rd(AIC_HWM + 0));
	wr(0x4000, 1u);
	sw_b = rd(0x4000);
	line_hex("SWb", sw_b);

	pmgr_wr(PMGR_GATE0 + PMGR_HWM_GATE * 4, before & ~0xFu);
	delay(DELAY_GATE);
	line_hex("H0q", rd(AIC_HWM + 0));
	line_hex("H3q", rd(AIC_HWM + 12));
	line_hex("H4q", rd(AIC_HWM + 16));
	wr(0x4080, 1u);
	wr(0x4000, 1u);
	sw_a = rd(0x4000);
	line_hex("SWa", sw_a);
	if (sw_b && !sw_a)
		line("4D kills AIC", WARN);
	else if (!rd(AIC_HWM + 0))
		line("4D HWM ok", ACC);
	line("phC done", DIM);
}
#endif /* v35: SMP-only */

/*
 * Phase D — v53: BLX iBEC bit1 plant @0x9FF1E5F9 + tramp/IPI.
 */
#define IB_TRAMP	0x800C3800u
#define IBEC_BIT1	0x9FF1E5F9u	/* Thumb: bit1 plant */
#define IBEC_GETTS	0x9FF1C305u	/* Thumb: timestamp helper */
#define DELAY_HALF	500000u
#define DELAY_LONG	2000000u
#define PMGR_CORE_PLANT0	0x6038u
#define PMGR_CORE_PLANT1	0x603Cu
#define PMGR_XNU_CORE_BASE	0x1000u
#define PMGR_XNU_CORE_MASK	0xfffffef0u
#define L2_REG		0x3E000C00u
#define L2_REG2		0x3E000C04u
#define MISC_3FB	0x3FB00000u
#define RST_3FD_08	0x3FD00008u
#define RST_3FD_0C	0x3FD0000Cu
#define RST_3FD_10	0x3FD00010u
#define SCU_CPU1_PWR	0x300u	/* bits [9:8] = 0b11 powered-off */

#if 0 /* v44–v51 Core Reset tables — orphaned iBSS path; kept for reference */
struct mmio_pair {
	u32 pa;
	u32 val;
};

/* table0: base[8]=0x3FE00000, base[9]=0x3FF00000 — all pairs in order. */
static const struct mmio_pair core_tbl0[] = {
	{ 0x3FE00008u, 0x00000808u },
	{ 0x3FE0000Cu, 0x00000808u },
	{ 0x3FE00028u, 0x00000808u },
	{ 0x3FE0002Cu, 0x00000808u },
	{ 0x3FE00048u, 0x00000404u },
	{ 0x3FE0004Cu, 0x00000404u },
	{ 0x3FE00068u, 0x00000808u },
	{ 0x3FE0006Cu, 0x00000808u },
	{ 0x3FE000A8u, 0x00000808u },
	{ 0x3FE000ACu, 0x00000404u },
	{ 0x3FF00408u, 0x00000000u },
	{ 0x3FF00408u, 0x01000001u },
	{ 0x3FF00408u, 0x02000002u },
	{ 0x3FF00408u, 0x03000003u },
	{ 0x3FF00408u, 0x04000004u },
	{ 0x3FF0040Cu, 0x00000000u },
	{ 0x3FF0040Cu, 0x01000001u },
	{ 0x3FF0040Cu, 0x02000002u },
	{ 0x3FF0040Cu, 0x03000003u },
	{ 0x3FF0040Cu, 0x04000004u },
	{ 0x3FF00428u, 0x00000000u },
	{ 0x3FF00428u, 0x01000001u },
	{ 0x3FF00428u, 0x02000002u },
	{ 0x3FF00428u, 0x03000003u },
	{ 0x3FF00428u, 0x04000004u },
	{ 0x3FF0042Cu, 0x00000000u },
	{ 0x3FF0042Cu, 0x01000001u },
	{ 0x3FF0042Cu, 0x02000002u },
	{ 0x3FF0042Cu, 0x03000003u },
	{ 0x3FF0042Cu, 0x04000004u },
	{ 0x3FE0001Cu, 0x00000003u },
	{ 0x3FE00030u, 0x00000100u },
};

/* table1 slices for v46 bisect (iBSS @0x34010568). */
static const struct mmio_pair core_t1_38d[] = {
	{ 0x38D00408u, 0x00000000u },
	{ 0x38D00408u, 0x01000001u },
	{ 0x38D00408u, 0x02000102u },
	{ 0x38D00408u, 0x03000103u },
	{ 0x38D0040Cu, 0x00000000u },
	{ 0x38D0040Cu, 0x01000001u },
	{ 0x38D0040Cu, 0x02000102u },
	{ 0x38D0040Cu, 0x03000103u },
};

static const struct mmio_pair core_t1_38ca[] = {
	{ 0x38C00004u, 0x00000808u },
	{ 0x38C00008u, 0x00000808u },
};

static const struct mmio_pair core_t1_38cb[] = {
	{ 0x38C0001Cu, 0x00000707u },
	{ 0x38C00020u, 0x00000404u },
	{ 0x38C00030u, 0x00000707u },
	{ 0x38C00034u, 0x00000404u },
};

static const struct mmio_pair core_t1_38cc[] = {
	{ 0x38C0000Cu, 0x01010101u },
	{ 0x38C00010u, 0x01010101u },
};

static int clock_gate_switch(u32 id, int on)
{
	u32 off, v, i;

	if (id > 78u || id == PMGR_HWM_GATE)
		return -1;
	off = PMGR_GATE0 + id * 4u;
	v = pmgr_rd(off);
	if (on)
		v |= 0xFu;
	else
		v &= ~0xFu;
	pmgr_wr(off, v);
	for (i = 0; i < 100000u; i++) {
		v = pmgr_rd(off);
		if (((v >> 4) & 0xFu) == (v & 0xFu))
			return 0;
	}
	return -1;
}

static void apply_mmio_table(const struct mmio_pair *t, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++)
		*(volatile u32 *)t[i].pa = t[i].val;
}
#endif /* orphaned Core Reset tables */

static void pause_half(const char *tag)
{
	line(tag, WARN);
	status(tag, 0);
	delay(DELAY_HALF);
}

static void mmio_wr32(u32 pa, u32 v)
{
	/* asm avoids -Warray-bounds on intentional phys-0 probes */
	__asm__ volatile("str %0, [%1]" : : "r"(v), "r"(pa) : "memory");
	__asm__ volatile("dsb" ::: "memory");
}

static u32 mmio_rd32(u32 pa)
{
	u32 v;

	__asm__ volatile("ldr %0, [%1]" : "=r"(v) : "r"(pa) : "memory");
	return v;
}

static void cache_clean_range(u32 pa, u32 nbytes)
{
	u32 p;

	for (p = pa & ~0x1Fu; p < pa + nbytes; p += 32u) {
		asm volatile("mcr p15, 0, %0, c7, c10, 1" : : "r"(p) : "memory");
		asm volatile("mcr p15, 0, %0, c7, c5, 1" : : "r"(p) : "memory");
	}
	asm volatile("mcr p15, 0, %0, c7, c5, 0" : : "r"(0) : "memory");
	asm volatile("mcr p15, 0, %0, c7, c5, 6" : : "r"(0) : "memory"); /* BPIALL */
	asm volatile("dsb\n isb" ::: "memory");
}

static int smp_wait_flag(void)
{
	u32 i;

	for (i = 0; i < 30u; i++) {
		if (secondary_flag == SEC_MAGIC)
			return 1;
		delay(50000u);
	}
	return 0;
}

static void smp_kick_ipi(void)
{
	wr(AIC_IPI_MASK_CLR, 0x80000001u);
	wr(0x5028u + (1u << 7), ~0u);
	wr(AIC_IPI_SEND, 1u << 1);
	__asm__ volatile("dsb; sev" ::: "memory");
}

#define IB_MAIL		0x800C3F00u
#define IB_ALIVE	0xC0DEBEEFu
#define IB_GO		0xC0DE0001u
#define IB_PLANT	0xC0DE0101u
#define IB_ENTRY_REG	0x38C00004u

/* PC-rel alive slot is last word of tramp image — find by scanning for 0 then
 * treating end-4; lab copies then searches for trailing zero word after code. */
static u32 tramp_alive_pa(void)
{
	u32 n = (u32)(secondary_park_tramp_end - secondary_park_tramp);

	/* park_alive_slot is the final word */
	return IB_TRAMP + (n - 1u) * 4u;
}

static void plant_park_tramp_from_lab(void)
{
	u32 *src = secondary_park_tramp;
	u32 n = (u32)(secondary_park_tramp_end - secondary_park_tramp);
	u32 i, pwr, r8z, r10z;
	static const u32 entry_offs[] = {
		0x38C00000u, 0x38C00004u, 0x38C00008u, 0x38C0000Cu,
		0x38C0001Cu, 0x38C00020u, 0x38C00030u, 0x38C00034u,
		0x38D00408u, 0x38D0040Cu,
	};

	r8z = mmio_rd32(RST_3FD_08);
	r10z = mmio_rd32(RST_3FD_10);
	pwr = g_cbar ? cbar_rd(SCU_PWR) : 0;
	line_hex("r8z", r8z);
	line_hex("r10z", r10z);
	line_hex("PWRz", pwr);

	/* ASSERT */
	mmio_wr32(RST_3FD_08, 0u);
	mmio_wr32(RST_3FD_0C, 0u);
	mmio_wr32(RST_3FD_10, 0u);
	if (g_cbar)
		cbar_wr(SCU_PWR, pwr | SCU_CPU1_PWR);
	delay(100000u);
	line_hex("r8a", mmio_rd32(RST_3FD_08));
	line_hex("PWRa", g_cbar ? cbar_rd(SCU_PWR) : 0);

	/* tramp + mailbox clear */
	for (i = 0; i < n; i++)
		mmio_wr32(IB_TRAMP + i * 4u, src[i]);
	mmio_wr32(IB_MAIL, 0u);
	mmio_wr32(IB_MAIL + 4u, 0u);
	mmio_wr32(IB_MAIL + 8u, IB_PLANT);
	mmio_wr32(tramp_alive_pa(), 0u);

	/* spray entry candidates (table1 / prior glass) */
	for (i = 0; i < sizeof(entry_offs) / sizeof(entry_offs[0]); i++)
		mmio_wr32(entry_offs[i], IB_TRAMP);
	pmgr_wr(0x6008u, IB_TRAMP);
	pmgr_wr(0x600Cu, IB_TRAMP);
	pmgr_wr(0x6010u, IB_TRAMP);

	/*
	 * phys0 reset vector: absolute ldr pc,[pc,#-4] + tramp literal.
	 * Survives even if VBAR=0 and stock E59FF018 table is ignored.
	 */
	mmio_wr32(0x00u, 0xE51FF004u);
	mmio_wr32(0x04u, IB_TRAMP);
	mmio_wr32(0x20u, IB_TRAMP); /* stock lit if E59FF018 still used */

	cache_clean_range(IB_TRAMP, n * 4u + 64u);
	cache_clean_range(IB_MAIL, 32u);
	cache_clean_range(0x00u, 64u);
	line_hex("eC4", mmio_rd32(IB_ENTRY_REG));
	line_hex("v00", mmio_rd32(0x00u));
	line_hex("v04", mmio_rd32(0x04u));
	delay(100000u);

	/* DEASSERT */
	mmio_wr32(RST_3FD_08, 0x808u);
	mmio_wr32(RST_3FD_0C, 0x808u);
	mmio_wr32(RST_3FD_10, 0x10u);
	if (g_cbar)
		cbar_wr(SCU_PWR, cbar_rd(SCU_PWR) & ~SCU_CPU1_PWR);
	__asm__ volatile("dsb; sev" ::: "memory");
	line_hex("r8d", mmio_rd32(RST_3FD_08));
	line_hex("PWRd", g_cbar ? cbar_rd(SCU_PWR) : 0);
}

#define ROM_STASH	0x80100000u
#define ROM_MAGIC	0xC0DED000u

/* USB egress mailbox + serial side-channel (host: labrecv8942.py). */
#define LAB_REPORT	0x800E0000u
#define LAB_MAGIC	0xC0DE05B1u	/* "USB" egress mailbox */
#define IBEC_RAM_LO	0x9FF00000u
#define IBEC_RAM_HI	0x9FFC0000u

static void lab_put_hex(char *dst, u32 v)
{
	static const char H[] = "0123456789ABCDEF";
	int i;

	for (i = 7; i >= 0; i--) {
		dst[i] = H[v & 0xfu];
		v >>= 4;
	}
}

static int lab_match_ascii(volatile u8 *p, const char *s)
{
	while (*s) {
		if (*p++ != (u8)*s++)
			return 0;
	}
	return 1;
}

static int lab_match_utf16le(volatile u8 *p, const char *s)
{
	while (*s) {
		if (p[0] != (u8)*s || p[1] != 0)
			return 0;
		p += 2;
		s++;
	}
	return 1;
}

static void lab_write_ascii(volatile u8 *p, const char *s)
{
	while (*s)
		*p++ = (u8)*s++;
}

static void lab_write_utf16le(volatile u8 *p, const char *s)
{
	while (*s) {
		*p++ = (u8)*s++;
		*p++ = 0;
	}
}

/*
 * Pack key results into LAB_REPORT and into any iBEC USB serial buffer
 * containing "CPID:" (ASCII or UTF-16LE). Host reads via irecovery -q /
 * pyusb get_string — no raw OTG, no iBSS park.
 */
static void lab_usb_publish(u32 flag, u32 tramp0, u32 plant, u32 tral)
{
	char msg[96];
	volatile u32 *rep = (volatile u32 *)LAB_REPORT;
	volatile u8 *p;
	u32 n;

	rep[0] = LAB_MAGIC;
	rep[1] = flag;
	rep[2] = tramp0;
	rep[3] = plant;
	rep[4] = tral;
	rep[5] = g_cbar;
	rep[6] = g_nr;
	rep[7] = 0x68u; /* lab version */
	cache_clean_range(LAB_REPORT, 64u);

	/* LAB:flag,tramp,plant,tral,cbar  — fits USB string limits */
	msg[0] = 'L';
	msg[1] = 'A';
	msg[2] = 'B';
	msg[3] = ':';
	lab_put_hex(msg + 4, flag);
	msg[12] = ',';
	lab_put_hex(msg + 13, tramp0);
	msg[21] = ',';
	lab_put_hex(msg + 22, plant);
	msg[30] = ',';
	lab_put_hex(msg + 31, tral);
	msg[39] = ',';
	lab_put_hex(msg + 40, g_cbar);
	msg[48] = 0;

	n = 0;
	/* Fast word scan — full byte walk is too slow on A5. */
	for (p = (volatile u8 *)IBEC_RAM_LO; p + 64 < (volatile u8 *)IBEC_RAM_HI; p += 4) {
		u32 w = *(volatile u32 *)p;
		/* ASCII "CPID" / "SRTG" little-endian */
		if (w == 0x44495043u || w == 0x47545253u) {
			lab_write_ascii(p, msg);
			n++;
			if (n >= 2u)
				break;
			continue;
		}
		/* UTF-16LE "CP" then need "ID:" — first halfword pair */
		if (w == 0x00500043u && lab_match_utf16le(p, "CPID:")) {
			lab_write_utf16le(p, msg);
			n++;
			if (n >= 2u)
				break;
		}
	}

	line_hex("usbN", n);
	if (n)
		line("USB LAB ok", ACC);
	else
		line("USB no CPID", WARN);
}

static void phase_romprobe(void)
{
	u32 i, w;

	line("--- ROM probe ---", ACC);
	line_hex("p0_0", mmio_rd32(0x00u));
	line_hex("p0_4", mmio_rd32(0x04u));
	line_hex("p0_18", mmio_rd32(0x18u));
	line_hex("hi0", mmio_rd32(0x10000000u));
	line_hex("hi4", mmio_rd32(0x10000004u));
	w = mmio_rd32(ROM_STASH);
	line_hex("st0", w);
	if (w == ROM_MAGIC) {
		line("iBSS stash", ACC);
		for (i = 1; i < 5u; i++)
			line_hex("st", mmio_rd32(ROM_STASH + i * 4u));
	} else {
		line("no ROM stash", WARN);
	}
	pause_half("rom");
}

static void phase_smp(void)
{
	u32 stub_pa, ibal, tramp0, plnt, tral, i;

	line("--- SMP v66 ---", ACC);
	status("phD ", 20);
	pause_half("p0 ok");

	secondary_flag = 0;
	stub_pa = (u32)(unsigned long)secondary_stub;
	line_hex("stub", stub_pa);
	cache_clean_range(stub_pa, 160u);

	line_hex("ibAL", mmio_rd32(IB_MAIL));
	line_hex("tr0", mmio_rd32(IB_TRAMP));

	line("lab plant", WARN);
	pause_half("plant");
	plant_park_tramp_from_lab();
	for (i = 0; i < 40u; i++) {
		ibal = mmio_rd32(IB_MAIL);
		tral = mmio_rd32(tramp_alive_pa());
		if (ibal == IB_ALIVE || tral == IB_ALIVE)
			break;
		delay(50000u);
	}
	tramp0 = mmio_rd32(IB_TRAMP);
	plnt = mmio_rd32(IB_MAIL + 8u);
	tral = mmio_rd32(tramp_alive_pa());
	ibal = mmio_rd32(IB_MAIL);
	line_hex("ibA2", ibal);
	line_hex("tr2", tramp0);
	line_hex("pln2", plnt);
	line_hex("trAL", tral);
	line_hex("eC4", mmio_rd32(IB_ENTRY_REG));
	line_hex("v00", mmio_rd32(0x00u));

	if (ibal != IB_ALIVE && tral != IB_ALIVE) {
		line("no cpu1 fetch", WARN);
		line_hex("flag", secondary_flag);
		line("phD done", DIM);
		lab_usb_publish(secondary_flag, tramp0, plnt, tral);
		pause_half("done");
		return;
	}

	line("park alive", ACC);
	pause_half("mail");
	mmio_wr32(IB_MAIL + 4u, stub_pa);
	cache_clean_range(IB_MAIL, 32u);
	mmio_wr32(IB_MAIL, IB_GO);
	cache_clean_range(IB_MAIL, 32u);
	__asm__ volatile("dsb; sev" ::: "memory");

	smp_kick_ipi();
	delay(DELAY_HALF);
	smp_kick_ipi();
	if (smp_wait_flag()) {
		line("CPU1 STUB OK", ACC);
		line("via mailbox", ACC);
	} else {
		line("CPU1 no stub", WARN);
		line_hex("EV1", rd(0x5084u));
	}
	line_hex("flag", secondary_flag);
	line("phD done", DIM);
	lab_usb_publish(secondary_flag, tramp0, plnt, tral);
	pause_half("done");
}

void main(void)
{
	u32 info_nr;

	wdt_kill();
	g_row = 0;
	g_col = 0;
	g_irq_n = g_spurious = g_step = 0;
	g_force_i = 1;
	g_cbar = 0;
	g_cap_done = 0;
	fb_text_fill(0, 0, FB_TEXT_W, FB_TEXT_H, BG);

	info_nr = rd(0x0004) & 0xffffu;
	g_nr = info_nr ? info_nr : 192u;
	g_words = (g_nr + 31u) >> 5;

	line("aic1-lab v68", ACC);
	line_hex("INFO", g_nr);
	g_cbar = read_cbar();
	line_hex("CBAR", g_cbar);
	status("boot", 0);
	pause_half("boot");

	phase_romprobe();
	phase_smp();

	line("ALL DONE", ACC);
	status("DONE", 0);
	/* WFI in _start — do not return (direct.dfu bx entry) */
}
