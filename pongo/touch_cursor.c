
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed short s16;

#define WDT_BASE     0x0F103020u
#define GPIO_BASE    0x3FA00000u
#define SPI1_BASE    0x32100000u
#define PMGR_BASE    0x3F100000u
#define PMGR_GATE0   0x1008u
#define CLK_EN_BITS  0x180u
#define SCANOUT      0x9F6FC000u
#define FW_BASE      0x80100000u
#define FB_W         768
#define FB_H         1024
#define STRIDE       3072
#define GPIO_ATTN    22
#define GPIO_CS      7
#define GPIO_RESET   5
#define I2C_SDA      4
#define I2C_SCL      5
#define PMU_I2C_ADDR 0x3cu
#define PMU_REG_ANA  0x020cu
#define PMU_REG_LDO  0x0213u
#define I2C0_BASE    0x33200000u
#define GPIO_I2C_CFG 0x00010102u /* ADT function-iic_* cell for pins 4/5 */
#define GPIO_RST_CFG 0x00010001u /* ADT function-reset on pin 5 */
#define GPIO_CS_CFG  0x00000101u /* ADT function-enable_cs on pin 7 */
#define GATE_PWM     83u
#define CLK_PWM      4u
#define PWM_BLK      0x33500300u /* ADT /arm-io/pwm reg */
#define GRAPE_CH     2u
#define PWM_TCFG0    0x00u
#define PWM_TCFG1    0x04u
#define PWM_TCON     0x08u
#define IIC_CON      0x00u
#define IIC_STAT     0x04u
#define IIC_DS       0x0Cu
#define GATE_I2C0    80u
#define GATE_SPI1    68u
#define CLK_I2C0     286u
#define CLK_SPI1A    304u
#define CLK_SPI1B    307u

#define SPI_CH_CFG   0x00
#define SPI_CLK_CFG  0x04
#define SPI_MODE_CFG 0x08
#define SPI_CS_REG   0x0C
#define SPI_INT_EN   0x10
#define SPI_STATUS   0x14
#define SPI_TX_DATA  0x18
#define SPI_RX_DATA  0x1C
#define SPI_PKT_CNT  0x20

#define CH_RXCH_ON   (1u << 1)
#define CH_TXCH_ON   (1u << 0)
#define CH_SW_RST    (1u << 5)
#define CS_SIG_INACT (1u << 0)
#define ENCLK_ENABLE (1u << 8)
#define CLKSEL_SRCMSK (3u << 9)
#define PACKET_CNT_EN (1u << 16)
#define MODE_BUS_BYTE      (0u << 17)
#define MODE_BUS_HALFWORD  (1u << 17)
#define MODE_CH_BYTE       (0u << 29)
#define MODE_CH_HALFWORD   (1u << 29)
#define MODE_TSZ_MASK      ((3u << 17) | (3u << 29))

#define UART_BASE    0x32500000u
#define UART_UTRSTAT 0x10u
#define A_SPI_CTRL   0x00u
#define A_SPI_SETUP  0x04u
#define A_SPI_STATUS 0x08u
#define A_SPI_TX     0x10u
#define A_SPI_RX     0x20u
#define A_SPI_CLK    0x30u
#define A_SPI_RXLIM  0x34u

#define Z2_CMD_READ_IRQ  0xEBu
#define Z2_REPLY_IRQ     0xE1u
#define Z2_FINGER_OFF    24
#define Z2_NFINGER_OFF   16
#define Z2_TOUCH_START   3
#define Z2_TOUCH_MOVED   4
#define Z2FW_MAGIC       0x5746325Au
#define LOAD_INIT        0u
#define LOAD_BLOB        1u
#define LOAD_CAL         2u

#define IBEC_LOAD      0x9FF00000u
#define IBEC_FN_CLOCK  (IBEC_LOAD + 0x2F55u) /* clock_set: r0=clk_id r1=enable */
#define IBEC_FN_GATE   (IBEC_LOAD + 0x1F1EDu) /* gate_switch: r0=gate r1=1 on */

#define BG       0xFF1A1A2Eu
#define FG       0xFFFFFFFFu
#define CURSOR   0xFFFFCC00u
#define DIM      0xFF808090u
#define HIT      0xFF00FF00u
#define MISS     0xFFFF0000u
#define WARN     0xFFFF8800u
#define ACC      0xFF00A0FFu

static volatile u32 *gpio = (volatile u32 *)GPIO_BASE;
static volatile u32 *g_spi = (volatile u32 *)SPI1_BASE;
static u8 tx_buf[64];
static u8 rx_buf[512];
static int parity;
static int cur_x = FB_W / 2, cur_y = FB_H / 2;
static int have_touch;
static u32 frames_ok, frames_fail, attn_lo, poll_n;
static u32 gpio22_raw, spi_ch, spi_st, last_rx0;
static u32 g_phase, g_boot_attn, g_fw_ok, g_has_fw, g_fw_size, g_fw_err;
static u32 g_gate_ok, g_pmu_st, g_i2c_st, g_gate80_raw, g_gate68_raw;
static u32 g_ibec_st;
static u32 g_clk304, g_clk307, g_clkpwm, g_spi_rb, g_rst_st;
static u32 g_spi_base, g_spi_kind, g_uart_peek, g_i2c_peek, g_pwm_peek;

static inline void wr32(volatile u32 *b, u32 off, u32 v) { b[off >> 2] = v; }
static inline u32 rd32(volatile u32 *b, u32 off) { return b[off >> 2]; }
static inline void delay(u32 n) { while (n--) __asm__ volatile("nop"); }
static inline u32 le32r(const u8 *p) {
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void v7_flush(void);
static void mmio_peers(void);
static void mmio_discover(void);
static void spi_init(void);
static void spi_cs(int active);
static int z2_read_packet(void);

static inline u32 read_sctlr(void)
{
	u32 v;

	__asm__ volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(v));
	return v;
}

static void wdt_kill(void)
{
	volatile u32 *w = (volatile u32 *)WDT_BASE;

	w[0] = w[1] = w[2] = w[3] = 0;
}

static u32 gpio_get(u32 pin) { return rd32(gpio, pin * 4) & 1u; }

static void gpio_set_out(u32 pin, u32 val)
{
	u32 v = rd32(gpio, pin * 4);

	if (val)
		v |= 1u;
	else
		v &= ~1u;
	wr32(gpio, pin * 4, v);
}

static u32 pmgr_rd(u32 off) { return *(volatile u32 *)(PMGR_BASE + off); }

static void pmgr_wr(u32 off, u32 v)
{
	*(volatile u32 *)(PMGR_BASE + off) = v;
	v7_flush();
}

static u32 pmgr_clk_addr(u32 id)
{
	u32 idx, off;

	if (id >> 24)
		return 0;
	idx = ((id >> 5) & 0x07fffff8u) | (id & 7u);
	off = (idx << 2) & 0x3fcu;
	return PMGR_BASE + off;
}

static int pmgr_clk_on(u32 id, u32 *raw)
{
	u32 addr, v, rb;

	addr = pmgr_clk_addr(id);
	if (!addr)
		return -1;
	v = *(volatile u32 *)addr;
	v = (v & ~CLK_EN_BITS) | CLK_EN_BITS;
	*(volatile u32 *)addr = v;
	v7_flush();
	rb = *(volatile u32 *)addr;
	if (raw)
		*raw = rb;
	if ((rb & CLK_EN_BITS) != CLK_EN_BITS)
		return -1;
	return 0;
}

static int pmgr_gate_xnu(u32 id, u32 nibble)
{
	u32 off, v, t;

	if (id > 0x57u)
		return -1;
	off = 0x1000u + id * 4u;
	v = pmgr_rd(off);
	v = (v & 0xFFFFFEF0u) | (nibble & 0xFu);
	pmgr_wr(off, v);
	for (t = 0; t < 500000u; t++) {
		v = pmgr_rd(off);
		if (((v ^ (v >> 4)) & 0xFu) == 0u)
			return 0;
	}
	return -1;
}

static int pmgr_gate_on(u32 id)
{
	u32 off, v, t, lo, hi;

	if (id > 78u)
		return -1;
	off = PMGR_GATE0 + id * 4u;
	v = pmgr_rd(off);
	v |= 0xFu;
	pmgr_wr(off, v);
	for (t = 0; t < 500000u; t++) {
		v = pmgr_rd(off);
		lo = v & 0xFu;
		hi = (v >> 4) & 0xFu;
		if (hi == lo && lo == 0xFu) {
			if (id == GATE_I2C0)
				g_gate80_raw = v;
			if (id == GATE_SPI1)
				g_gate68_raw = v;
			return 0;
		}
	}
	v = pmgr_rd(off);
	if (id == GATE_I2C0)
		g_gate80_raw = v;
	if (id == GATE_SPI1)
		g_gate68_raw = v;
	lo = v & 0xFu;
	hi = (v >> 4) & 0xFu;
	if (hi == lo && lo == 0xFu)
		return 0;
	return -1;
}

static inline volatile u32 *iic_reg(u32 off)
{
	return (volatile u32 *)(I2C0_BASE + off);
}

static int i2c_hw_wait(u32 n)
{
	while (n--) {
		if (iic_reg(IIC_CON)[0] & 0x10u)
			return 0;
		delay(1);
	}
	return -1;
}

static int i2c_hw_init(void)
{
	iic_reg(IIC_CON)[0] = 0xb7u;
	iic_reg(IIC_STAT)[0] = 0x10u;
	return 0;
}

static int i2c_hw_start(u8 addr, int tx)
{
	iic_reg(IIC_DS)[0] = (u32)((addr << 1) | (tx ? 0u : 1u));
	iic_reg(IIC_STAT)[0] = 0xf0u;
	iic_reg(IIC_CON)[0] = 0xb7u;
	if (i2c_hw_wait(500000u))
		return -1;
	if (iic_reg(IIC_STAT)[0] & 1u)
		return -2;
	return 0;
}

static int i2c_hw_tx(u8 b)
{
	iic_reg(IIC_DS)[0] = b;
	iic_reg(IIC_CON)[0] = 0xb7u;
	if (i2c_hw_wait(500000u))
		return -1;
	if (iic_reg(IIC_STAT)[0] & 1u)
		return -2;
	return 0;
}

static int i2c_hw_rx(u8 *b, int ack)
{
	iic_reg(IIC_CON)[0] = ack ? 0xb7u : 0x37u;
	if (i2c_hw_wait(500000u))
		return -1;
	*b = (u8)iic_reg(IIC_DS)[0];
	return 0;
}

static void i2c_hw_stop(void)
{
	iic_reg(IIC_STAT)[0] = 0x90u;
	iic_reg(IIC_CON)[0] = 0xb7u;
	delay(200);
	iic_reg(IIC_STAT)[0] = 0u;
}

static int i2c_hw_txn(u8 addr, const u8 *w, u32 wlen, u8 *r, u32 rlen)
{
	u32 i;
	int rc;

	rc = i2c_hw_start(addr, 1);
	if (rc)
		goto out;
	for (i = 0; i < wlen; i++) {
		rc = i2c_hw_tx(w[i]);
		if (rc)
			goto out;
	}
	if (rlen) {
		rc = i2c_hw_start(addr, 0);
		if (rc)
			goto out;
		for (i = 0; i < rlen; i++) {
			rc = i2c_hw_rx(r + i, i + 1u < rlen);
			if (rc)
				goto out;
		}
	}
	rc = 0;
out:
	i2c_hw_stop();
	return rc;
}

static volatile u32 *gpio_sam(u32 pin)
{
	return (volatile u32 *)(GPIO_BASE + pin * 0x20u);
}

static void i2c_bb_delay(void) { delay(30); }

static void sam_sda_mode(int out)
{
	volatile u32 *p = gpio_sam(I2C_SDA);

	p[0] = out ? 1u : 0u;
}

static void sam_scl(u32 hi)
{
	volatile u32 *p = gpio_sam(I2C_SCL);

	p[0] = 1u;
	if (hi)
		p[1] |= 1u;
	else
		p[1] &= ~1u;
	i2c_bb_delay();
}

static void sam_sda_out(u32 hi)
{
	volatile u32 *p = gpio_sam(I2C_SDA);

	sam_sda_mode(1);
	if (hi)
		p[1] |= 1u;
	else
		p[1] &= ~1u;
	i2c_bb_delay();
}

static u32 sam_sda_in(void)
{
	volatile u32 *p = gpio_sam(I2C_SDA);

	sam_sda_mode(0);
	return p[1] & 1u;
}


static void i2c_bb_scl(u32 hi)
{
	sam_scl(hi);
}

static void i2c_bb_sda(u32 hi)
{
	sam_sda_out(hi);
}

static u32 i2c_bb_sda_read(void)
{
	return sam_sda_in();
}

static void i2c_bb_start(void)
{
	i2c_bb_sda(1);
	i2c_bb_scl(1);
	i2c_bb_sda(0);
	i2c_bb_scl(0);
}

static void i2c_bb_stop(void)
{
	i2c_bb_sda(0);
	i2c_bb_scl(1);
	i2c_bb_sda(1);
}

static int i2c_bb_write_byte(u8 b)
{
	u32 i, ack;

	for (i = 0; i < 8u; i++) {
		i2c_bb_sda((b >> (7u - i)) & 1u);
		i2c_bb_scl(1);
		i2c_bb_scl(0);
	}
	i2c_bb_sda(1);
	i2c_bb_scl(1);
	ack = i2c_bb_sda_read();
	i2c_bb_scl(0);
	return ack ? -1 : 0;
}

static int i2c_bb_read_byte(u8 *b, int ack)
{
	u32 i, bit, v = 0;

	for (i = 0; i < 8u; i++) {
		i2c_bb_scl(1);
		bit = i2c_bb_sda_read();
		i2c_bb_scl(0);
		v = (v << 1) | bit;
	}
	*b = (u8)v;
	i2c_bb_sda(ack ? 0u : 1u);
	i2c_bb_scl(1);
	i2c_bb_scl(0);
	i2c_bb_sda(1);
	return 0;
}

static int i2c_bb_txn(u8 addr, const u8 *w, u32 wlen, u8 *r, u32 rlen)
{
	u32 i;

	i2c_bb_start();
	if (i2c_bb_write_byte(addr << 1))
		goto fail;
	for (i = 0; i < wlen; i++) {
		if (i2c_bb_write_byte(w[i]))
			goto fail;
	}
	if (rlen) {
		i2c_bb_start();
		if (i2c_bb_write_byte((addr << 1) | 1u))
			goto fail;
		for (i = 0; i < rlen; i++) {
			if (i2c_bb_read_byte(r + i, i + 1u < rlen))
				goto fail;
		}
	}
	i2c_bb_stop();
	return 0;
fail:
	i2c_bb_stop();
	return -1;
}

static int i2c_txn(u8 addr, const u8 *w, u32 wlen, u8 *r, u32 rlen)
{
	if (g_i2c_st & 0x100u)
		return i2c_hw_txn(addr, w, wlen, r, rlen);
	return i2c_bb_txn(addr, w, wlen, r, rlen);
}

static int pmu_read8(u16 reg, u8 *val)
{
	u8 cmd[2];

	cmd[0] = (u8)(reg >> 8);
	cmd[1] = (u8)(reg & 0xff);
	return i2c_txn(PMU_I2C_ADDR, cmd, 2, val, 1);
}

static int pmu_write8(u16 reg, u8 val)
{
	u8 cmd[3];

	cmd[0] = (u8)(reg >> 8);
	cmd[1] = (u8)(reg & 0xff);
	cmd[2] = val;
	return i2c_txn(PMU_I2C_ADDR, cmd, 3, 0, 0);
}

static int pmu_write8_lo(u8 reg, u8 val)
{
	u8 cmd[2];

	cmd[0] = reg;
	cmd[1] = val;
	return i2c_txn(PMU_I2C_ADDR, cmd, 2, 0, 0);
}

static int pmu_ldo_on16(u16 reg)
{
	u8 val;

	if (!pmu_read8(reg, &val))
		return pmu_write8(reg, (u8)(val | 0x01u));
	return pmu_write8(reg, 0x01u);
}

static int pmu_ldo_on8(u8 reg)
{
	u8 val = 0;

	if (!i2c_txn(PMU_I2C_ADDR, &reg, 1, &val, 1))
		return pmu_write8_lo(reg, (u8)(val | 0x01u));
	return pmu_write8_lo(reg, 0x01u);
}

static int pmu_ldo_on(u16 reg)
{
	if (!pmu_ldo_on16(reg))
		return 0;
	return pmu_ldo_on8((u8)(reg & 0xffu));
}

static void mt_i2c_gpio_init(void)
{
	wr32(gpio, I2C_SDA * 4, GPIO_I2C_CFG);
	wr32(gpio, I2C_SCL * 4, GPIO_I2C_CFG);
	delay(2000);
}

static void mt_gpio_cs_pin(void)
{
	/* ADT function-enable_cs + function-spi_cs0 (pin 7 + pin 4 mux). */
	wr32(gpio, GPIO_CS * 4, GPIO_CS_CFG);
	wr32(gpio, 4 * 4, 0x00010001u);
	delay(500);
}

static void mt_gpio_reset_pin(void)
{
	wr32(gpio, GPIO_RESET * 4, GPIO_RST_CFG);
	delay(500);
}

static int i2c_bb_probe(u8 addr)
{
	i2c_bb_start();
	if (i2c_bb_write_byte(addr << 1)) {
		i2c_bb_stop();
		return -1;
	}
	i2c_bb_stop();
	return 0;
}

static int mt_i2c_probe(void)
{
	volatile u32 *scl = gpio_sam(I2C_SCL);
	int rc;

	mt_i2c_gpio_init();
	i2c_hw_init();
	g_i2c_st |= 1u;
	rc = i2c_hw_start(PMU_I2C_ADDR, 1);
	i2c_hw_stop();
	if (!rc) {
		g_i2c_st |= 0x100u;
		return 0;
	}
	g_i2c_st |= (u32)((rc & 0xfu) << 4);
	scl[0] = 1u;
	scl[1] |= 1u;
	sam_sda_out(1);
	if (!i2c_bb_probe(PMU_I2C_ADDR)) {
		g_i2c_st |= 0x200u;
		return 0;
	}
	g_i2c_st |= 0x2000u;
	return -1;
}

static void mt_pmu_clock_pre(void)
{
	/* ADT function-clock_enable-pmu: OIPG on pmu — best-effort wake. */
	(void)pmu_write8(0x0200u, 0x01u);
	delay(5000);
	(void)pmu_write8(0x0201u, 0x02u);
	delay(5000);
}

static void pmgr_bootstrap(void)
{
	u32 v;

	/* XNU AppleS5L8940XIO::_initPMGRState replay (see docs/p105-xnu-pmgr-extract.md). */
	v = pmgr_rd(0x1180u);
	pmgr_wr(0x1180u, v | 0x80000000u);
	pmgr_wr(0x1200u, 0x7FFEu);
	pmgr_wr(0x1204u, 0x3fff8001u);
	pmgr_wr(0x1010u, 0x0014000fu);
	pmgr_wr(0x1014u, 0x0014000fu);
	pmgr_wr(0x1018u, 0x0014000fu);
	pmgr_wr(0x107Cu, 0x00100000u);
}

static inline u32 pwm_tcon_chan(u32 ch)
{
	return (ch == 0u) ? 0u : (ch + 1u);
}

static void mt_grape_clk_on(void)
{
	static const u32 pwm_bases[] = { PWM_BLK, 0x33500000u, 0x33500500u };
	volatile u32 *pwm = (volatile u32 *)PWM_BLK;
	u32 tc = pwm_tcon_chan(GRAPE_CH);
	u32 cnt = 732u; /* 32768 Hz @ ~24 MHz tin, /256 prescaler */
	u32 tcfg0, tcfg1, tcon, tcnt_off = 0x0cu + GRAPE_CH * 0x0cu;
	u32 i;

	tcfg0 = pwm[PWM_TCFG0 >> 2];
	tcfg0 = (tcfg0 & 0x00ffffffu) | (0xffu << 8);
	pwm[PWM_TCFG0 >> 2] = tcfg0;
	tcfg1 = pwm[PWM_TCFG1 >> 2];
	tcfg1 &= ~(0xfu << (GRAPE_CH * 4u));
	pwm[PWM_TCFG1 >> 2] = tcfg1;
	pwm[tcnt_off >> 2] = cnt;
	pwm[(tcnt_off + 4u) >> 2] = cnt / 2u;
	tcon = pwm[PWM_TCON >> 2];
	tcon |= (1u << (tc * 4u + 1u));
	pwm[PWM_TCON >> 2] = tcon;
	tcon &= ~(1u << (tc * 4u + 1u));
	tcon |= (1u << (tc * 4u + 0u)) | (1u << (tc * 4u + 3u));
	pwm[PWM_TCON >> 2] = tcon;
	v7_flush();
	delay(5000);
	g_pwm_peek = pwm[PWM_TCON >> 2];
	for (i = 0; i < 3u && !g_pwm_peek; i++) {
		pwm = (volatile u32 *)pwm_bases[i];
		g_pwm_peek = pwm[PWM_TCON >> 2];
	}
	if (g_pwm_peek)
		g_gate_ok |= 0x80u;
}

static void mt_ibec_clocks(void)
{
	typedef void (*ibec_clock_set_t)(u32 id, int en);
	typedef void (*ibec_gate_t)(u32 id, u32 en);
	static const u32 clk_ids[] = { CLK_I2C0, CLK_SPI1A, CLK_SPI1B, CLK_PWM };
	static const u32 gate_ids[] = { GATE_I2C0, GATE_SPI1, GATE_PWM };
	const u16 *probe = (const u16 *)(IBEC_LOAD + 0x2F54u);
	ibec_clock_set_t cs;
	ibec_gate_t gs;
	u32 i;

	if (probe[0] != 0xB590u) {
		g_ibec_st = 0xBAD00001u;
		return;
	}
	g_ibec_st = 0x1BEC0000u;
	cs = (ibec_clock_set_t)IBEC_FN_CLOCK;
	gs = (ibec_gate_t)IBEC_FN_GATE;
	for (i = 0; i < 4u; i++) {
		cs(clk_ids[i], 1);
		g_ibec_st |= (1u << i);
	}
	for (i = 0; i < 3u; i++) {
		gs(gate_ids[i], 1u);
		g_ibec_st |= (0x10u << i);
	}
}

static void mt_clocks_on(void)
{
	if (!pmgr_clk_on(CLK_I2C0, 0))
		g_gate_ok |= 4u;
	if (!pmgr_clk_on(CLK_SPI1A, &g_clk304))
		g_gate_ok |= 8u;
	if (!pmgr_clk_on(CLK_SPI1B, &g_clk307))
		g_gate_ok |= 0x10u;
	if (!pmgr_clk_on(CLK_PWM, &g_clkpwm))
		g_gate_ok |= 0x40u;
	if (!pmgr_gate_xnu(GATE_I2C0, 0xFu))
		g_gate_ok |= 0x100u;
	if (!pmgr_gate_xnu(GATE_SPI1, 0xFu))
		g_gate_ok |= 0x200u;
	if (!pmgr_gate_xnu(GATE_PWM, 0xFu))
		g_gate_ok |= 0x400u;
	if (!pmgr_gate_on(GATE_I2C0))
		g_gate_ok |= 1u;
	if (!pmgr_gate_on(GATE_SPI1))
		g_gate_ok |= 2u;
	if (!pmgr_gate_on(GATE_PWM))
		g_gate_ok |= 0x20u;
}

static void mt_pmu_power(void)
{
	if (mt_i2c_probe())
		return;
	mt_pmu_clock_pre();
	if (!pmu_ldo_on(PMU_REG_ANA))
		g_pmu_st |= 1u;
	delay(20000);
	if (!pmu_ldo_on(PMU_REG_LDO))
		g_pmu_st |= 2u;
	delay(50000);
	mt_gpio_reset_pin();
	mt_gpio_cs_pin();
	gpio_set_out(GPIO_CS, 1);
}

static void mt_power_up(void)
{
	g_phase = 0;
	mt_ibec_clocks();
	mt_clocks_on();
	pmgr_bootstrap();
	mt_grape_clk_on();
	mt_pmu_power();
	mmio_discover();
	spi_init();
	mmio_peers();
}

static void fb_pixel(int x, int y, u32 c)
{
	if ((u32)x >= FB_W || (u32)y >= FB_H)
		return;
	*(volatile u32 *)(SCANOUT + y * STRIDE + x * 4) = c;
}

static void fb_fill(int x, int y, int w, int h, u32 c)
{
	int i, j;

	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++)
			fb_pixel(x + i, y + j, c);
}

static void fb_hline(int x, int y, int w, u32 c)
{
	int i;

	for (i = 0; i < w; i++)
		fb_pixel(x + i, y, c);
}

static void fb_vline(int x, int y, int h, u32 c)
{
	int i;

	for (i = 0; i < h; i++)
		fb_pixel(x, y + i, c);
}

static void draw_cursor(int x, int y, u32 c)
{
	fb_hline(x - 12, y, 25, c);
	fb_vline(x, y - 12, 25, c);
	fb_fill(x - 2, y - 2, 5, 5, c);
}

#include "fb_text8.h"

#define draw_hex_line(x, y, v, c) draw_u32_hex8(x, y, v, c, 2)

static void ui_line(int y, const char *lab, u32 v, u32 c)
{
	draw_string8(8, y, lab, DIM, 2);
	draw_u32_hex8(88, y, v, c, 2);
}

static void v7_flush(void)
{
	__asm__ volatile("dsb");
	__asm__ volatile("isb");
}

static void spi_wr(u32 off, u32 v)
{
	wr32(g_spi, off, v);
	v7_flush();
}

static u32 spi_rx_level(u32 st)
{
	u32 n;

	n = (st >> 13) & 0x7fu;
	if (n)
		return n;
	n = (st >> 15) & 0x1ffu;
	if (n)
		return n;
	return (st & 2u) ? 1u : 0u;
}

static void spi_set_bpw(u32 bpw)
{
	u32 mode = rd32(g_spi, SPI_MODE_CFG);

	mode &= ~MODE_TSZ_MASK;
	if (bpw == 16) {
		mode |= MODE_BUS_HALFWORD | MODE_CH_HALFWORD;
	} else {
		mode |= MODE_BUS_BYTE | MODE_CH_BYTE;
	}
	spi_wr(SPI_MODE_CFG, mode);
}

static void spi_flush_fifo(void)
{
	u32 t = 100000;

	spi_wr(SPI_PKT_CNT, 0);
	while (t--) {
		u32 st = rd32(g_spi, SPI_STATUS);

		if (spi_rx_level(st))
			(void)rd32(g_spi, SPI_RX_DATA);
		else
			break;
	}
}

static u32 apple_spi_wake(u32 base)
{
	volatile u32 *r = (volatile u32 *)base;

	r[A_SPI_STATUS >> 2] = 0xfu;
	r[A_SPI_CTRL >> 2] = r[A_SPI_CTRL >> 2] | 0xcu;
	r[A_SPI_CLK >> 2] = 4u;
	r[SPI_CS_REG >> 2] = 6u;
	r[A_SPI_SETUP >> 2] = 0x10618u;
	r[A_SPI_CTRL >> 2] = 1u;
	v7_flush();
	delay(5000);
	return r[A_SPI_STATUS >> 2] | r[A_SPI_CTRL >> 2] | r[A_SPI_SETUP >> 2];
}

static void mmio_peers(void)
{
	g_uart_peek = *(volatile u32 *)(UART_BASE + UART_UTRSTAT);
	g_i2c_peek = *(volatile u32 *)(I2C0_BASE + IIC_CON);
	g_pwm_peek = *(volatile u32 *)(PWM_BLK + PWM_TCON);
}

static u32 spi_region_tag(u32 base)
{
	u32 off, v;

	for (off = 0; off < 0x100u; off += 4u) {
		v = *(volatile u32 *)(base + off);
		if (v)
			return (off << 8) | (v & 0xffu);
	}
	return 0;
}

static void spi1_hw_reset(void)
{
	volatile u32 *r = (volatile u32 *)SPI1_BASE;
	u32 v;

	v = r[SPI_CH_CFG >> 2];
	r[SPI_CH_CFG >> 2] = v | CH_SW_RST;
	v7_flush();
	delay(2000);
	r[SPI_CH_CFG >> 2] = v & ~CH_SW_RST;
	v7_flush();
	delay(2000);
}

static int spi_try_samsung(u32 base, u32 *tag)
{
	u32 pre, rb;

	pre = *(volatile u32 *)(base + SPI_CH_CFG);
	*(volatile u32 *)(base + SPI_CS_REG) = CS_SIG_INACT;
	v7_flush();
	rb = *(volatile u32 *)(base + SPI_CS_REG);
	if (rb) {
		*tag = (rb << 8) | (pre & 0xffu);
		return 1;
	}
	rb = spi_region_tag(base);
	if (rb) {
		*tag = (0xEu << 28) | rb;
		return 1;
	}
	return 0;
}

static void mmio_discover(void)
{
	u32 tag = 0, rb;

	mmio_peers();
	g_spi = (volatile u32 *)SPI1_BASE;
	g_spi_base = SPI1_BASE;
	g_spi_kind = 0u;
	spi1_hw_reset();
	if (spi_try_samsung(SPI1_BASE, &tag)) {
		g_spi_rb = tag;
		g_rst_st |= 0x40u;
		return;
	}
	rb = apple_spi_wake(SPI1_BASE);
	if (rb) {
		g_spi_kind = 1u;
		g_spi_rb = (1u << 24) | (rb & 0xfffffu);
		g_rst_st |= 0x40u;
	}
}

static void apple_spi_init(void)
{
	(void)apple_spi_wake(g_spi_base);
	spi_ch = rd32(g_spi, A_SPI_CTRL);
	spi_st = rd32(g_spi, A_SPI_STATUS);
	g_spi_rb = (1u << 24) | (spi_st << 8) | (spi_ch & 0xffu);
}

static int apple_spi_xfer_raw(const u8 *tx, u8 *rx, u32 len, u32 bpw)
{
	u32 i, step, txw, st, deadline;

	if (bpw == 16 && (len & 1u))
		return -1;
	step = (bpw == 16) ? 2u : 1u;
	spi_cs(1);
	for (i = 0; i < len; i += step) {
		txw = 0xffu;
		if (tx) {
			if (bpw == 16)
				txw = (u32)tx[i] | ((u32)tx[i + 1] << 8);
			else
				txw = tx[i];
		}
		spi_wr(A_SPI_RXLIM, 1u);
		deadline = 500000u;
		while (((st = rd32(g_spi, A_SPI_STATUS)) & 0x1f0u) == 0x100u) {
			if (!deadline--)
				goto fail;
		}
		spi_wr(A_SPI_TX, txw);
		deadline = 500000u;
		while (!(rd32(g_spi, A_SPI_STATUS) & 0x3e00u)) {
			if (!deadline--)
				goto fail;
		}
		st = rd32(g_spi, A_SPI_RX);
		if (rx) {
			if (bpw == 16) {
				rx[i] = (u8)(st & 0xffu);
				rx[i + 1] = (u8)(st >> 8);
			} else {
				rx[i] = (u8)st;
			}
		}
	}
	spi_cs(0);
	return 0;
fail:
	spi_cs(0);
	return -1;
}

static void spi_probe_mmio(void)
{
	u32 rb;

	spi_wr(SPI_CS_REG, CS_SIG_INACT);
	rb = rd32(g_spi, SPI_CS_REG);
	spi_ch = rd32(g_spi, SPI_CH_CFG);
	spi_st = rd32(g_spi, SPI_STATUS);
	g_spi_rb = rb | (spi_ch << 8) | (spi_st << 16);
}

static void spi_init(void)
{
	if (g_spi_kind == 1u) {
		apple_spi_init();
		return;
	}
	u32 v, mode, pre;

	pre = rd32(g_spi, SPI_CH_CFG);
	if (pre) {
		v = pre | CH_SW_RST;
		spi_wr(SPI_CH_CFG, v);
		delay(1000);
	}
	spi_wr(SPI_INT_EN, 0);
	spi_wr(SPI_PKT_CNT, 0);
	v = rd32(g_spi, SPI_CH_CFG);
	v &= ~CH_SW_RST;
	spi_wr(SPI_CH_CFG, v);
	v = rd32(g_spi, SPI_CLK_CFG);
	v |= ENCLK_ENABLE;
	v &= ~CLKSEL_SRCMSK;
	v = (v & ~0xffu) | 0x0fu;
	spi_wr(SPI_CLK_CFG, v);
	mode = (0x3ffu << 19);
	spi_wr(SPI_MODE_CFG, mode);
	v = rd32(g_spi, SPI_CH_CFG);
	v &= ~((1u << 4) | (1u << 3) | (1u << 2));
	spi_wr(SPI_CH_CFG, v);
	spi_wr(SPI_CS_REG, CS_SIG_INACT);
	spi_flush_fifo();
	spi_set_bpw(8);
	spi_probe_mmio();
}

static void spi_cs(int active)
{
	gpio_set_out(GPIO_CS, active ? 0 : 1);
	if (g_spi_kind == 1u)
		return;
	if (active)
		spi_wr(SPI_CS_REG, 0);
	else
		spi_wr(SPI_CS_REG, CS_SIG_INACT);
}

static int spi_wait_rx_units(u32 want)
{
	u32 deadline = 500000;

	while (deadline--) {
		if (spi_rx_level(rd32(g_spi, SPI_STATUS)) >= want)
			return 0;
	}
	return -1;
}

static int spi_xfer_raw(const u8 *tx, u8 *rx, u32 len, u32 bpw)
{
	if (g_spi_kind == 1u)
		return apple_spi_xfer_raw(tx, rx, len, bpw);

	u32 i, ch, units, pkt;
	const u8 *tp = tx;
	u8 *rp = rx;

	if (bpw == 16 && (len & 1u))
		return -1;
	if (bpw == 16) {
		units = len >> 1;
		pkt = (len << 2) & 0xffffu;
	} else {
		units = len;
		pkt = (len << 3) & 0xffffu;
	}
	spi_set_bpw(bpw);
	spi_cs(1);
	delay(50);
	spi_wr(SPI_PKT_CNT, 0);
	spi_wr(SPI_PKT_CNT, PACKET_CNT_EN | pkt);
	ch = rd32(g_spi, SPI_CH_CFG);
	ch |= CH_TXCH_ON | CH_RXCH_ON;
	spi_wr(SPI_CH_CFG, ch);
	for (i = 0; i < units; i++) {
		u32 txw = 0xff;

		if (tp) {
			if (bpw == 16) {
				txw = (u32)tp[0] | ((u32)tp[1] << 8);
				tp += 2;
			} else {
				txw = *tp++;
			}
		}
		spi_wr(SPI_TX_DATA, txw);
	}
	if (spi_wait_rx_units(units)) {
		ch = rd32(g_spi, SPI_CH_CFG);
		ch &= ~(CH_TXCH_ON | CH_RXCH_ON);
		spi_wr(SPI_CH_CFG, ch);
		spi_cs(0);
		spi_set_bpw(8);
		return -1;
	}
	for (i = 0; i < units; i++) {
		u32 d = rd32(g_spi, SPI_RX_DATA);

		if (rp) {
			if (bpw == 16) {
				*rp++ = (u8)(d & 0xff);
				*rp++ = (u8)(d >> 8);
			} else {
				*rp++ = (u8)d;
			}
		}
	}
	ch = rd32(g_spi, SPI_CH_CFG);
	ch &= ~(CH_TXCH_ON | CH_RXCH_ON);
	spi_wr(SPI_CH_CFG, ch);
	spi_cs(0);
	delay(50);
	spi_set_bpw(8);
	return 0;
}

static int spi_selftest(void)
{
	u8 tx[4] = { 0xff, 0xff, 0xff, 0xff };
	u8 rx[4] = { 0, 0, 0, 0 };
	int rc;

	rc = spi_xfer_raw(tx, rx, 4, 8);
	spi_ch = rd32(g_spi, SPI_CH_CFG);
	spi_st = rd32(g_spi, SPI_STATUS);
	return !rc;
}

static int wait_attn_low(u32 ms)
{
	u32 t = ms * 500u;

	while (t--) {
		if (gpio_get(GPIO_ATTN) == 0)
			return 1;
		delay(200);
	}
	return 0;
}

static int z2_send_fw_blob(const u8 *data, u32 len, int init)
{
	u8 ack[2] = { 0x1a, 0xa1 };
	u32 bpw = init ? 8u : 16u;

	if (spi_xfer_raw(data, 0, len, bpw)) {
		if (g_fw_err == 0u)
			g_fw_err = 2u;
		return -1;
	}
	if (spi_xfer_raw(ack, 0, 2, 8)) {
		if (g_fw_err == 0u)
			g_fw_err = 3u;
		return -2;
	}
	(void)wait_attn_low(25);
	return 0;
}

static u32 fw_size_at(u32 base)
{
	volatile u32 *p = (volatile u32 *)base;

	if (p[0] != Z2FW_MAGIC || p[1] != 1u)
		return 0;
	return 1; /* present — walker computes span */
}

static int z2_upload_firmware(u32 base)
{
	const u8 *fw = (const u8 *)base;
	u32 idx, fw_size, end;
	u32 cmd, size, addr;

	if (!fw_size_at(base))
		return -1;
	/* upper bound: scan until invalid cmd or 512 KiB */
	idx = 8;
	end = 8;
	while (idx + 8 <= 512u * 1024u) {
		cmd = le32r(fw + idx);
		idx += 4;
		if (cmd == LOAD_INIT || cmd == LOAD_BLOB) {
			if (idx + 4 > 512u * 1024u)
				break;
			size = le32r(fw + idx);
			idx += 4;
			if (size == 0 || size > 256u * 1024u || idx + size > 512u * 1024u)
				break;
			if (z2_send_fw_blob(fw + idx, size, cmd == LOAD_INIT))
				return -2;
			idx += size;
			end = idx;
		} else if (cmd == LOAD_CAL) {
			if (idx + 4 > 512u * 1024u)
				break;
			addr = le32r(fw + idx);
			idx += 4;
			(void)addr;
			end = idx;
		} else {
			break;
		}
		idx = (idx + 3u) & ~3u;
	}
	fw_size = end;
	if (fw_size <= 8)
		return -3;
	g_fw_size = fw_size;
	return 0;
}

static int mt_reset_and_boot(void)
{
	int up_rc;

	g_phase = 1;
	mt_gpio_reset_pin();
	mt_gpio_cs_pin();
	gpio_set_out(GPIO_CS, 1);
	gpio_set_out(GPIO_RESET, 1);
	delay(10000);
	spi_init();

	if (spi_selftest())
		g_rst_st |= 0x10u;

	/* Path A (apple_z2): assert reset, wait ATTN */
	gpio_set_out(GPIO_RESET, 0);
	delay(50000);
	if (wait_attn_low(100))
		g_rst_st |= 1u;

	/* Path B: release reset edge if A silent */
	if (!(g_rst_st & 1u)) {
		gpio_set_out(GPIO_RESET, 1);
		delay(50000);
		if (wait_attn_low(100))
			g_rst_st |= 2u;
	}

	g_boot_attn = (g_rst_st & 3u) ? 1u : 0u;
	g_phase = 2;

	if (!g_has_fw)
		return 0;

	/* Upload while held in reset (linux apple_z2 path). */
	up_rc = z2_upload_firmware(FW_BASE);
	if (!up_rc)
		goto boot_ok;

	/* Fallback: released reset. */
	gpio_set_out(GPIO_RESET, 1);
	delay(50000);
	up_rc = z2_upload_firmware(FW_BASE);
	if (!up_rc) {
		g_rst_st |= 4u;
		goto boot_ok;
	}

	/* Last try: re-assert reset. */
	gpio_set_out(GPIO_RESET, 0);
	delay(50000);
	up_rc = z2_upload_firmware(FW_BASE);
	if (up_rc) {
		if (g_fw_err == 0u)
			g_fw_err = 1u;
		gpio_set_out(GPIO_RESET, 1);
		return -1;
	}

boot_ok:
	g_fw_ok = 1;
	g_rst_st |= 8u;
	gpio_set_out(GPIO_RESET, 1);
	if (!g_boot_attn)
		g_boot_attn = wait_attn_low(100) ? 1u : 0u;

	parity = 0;
	(void)z2_read_packet();
	return 0;
}

static int z2_read_packet(void)
{
	int i;
	u16 pkt_len;
	u8 nf;
	const u8 *msg;
	s16 ax, ay;

	for (i = 0; i < 16; i++)
		tx_buf[i] = 0;
	tx_buf[0] = Z2_CMD_READ_IRQ;
	tx_buf[1] = (u8)(parity + 1);
	{
		u16 cs = (u16)(Z2_CMD_READ_IRQ + tx_buf[1]);

		tx_buf[14] = (u8)(cs & 0xff);
		tx_buf[15] = (u8)(cs >> 8);
	}
	parity = !parity;
	spi_xfer_raw(tx_buf, rx_buf, 16, 8);
	last_rx0 = rx_buf[0];
	if (rx_buf[0] != Z2_REPLY_IRQ && rx_buf[0] != Z2_CMD_READ_IRQ)
		return -1;
	pkt_len = (u16)(rx_buf[1] | (rx_buf[2] << 8));
	pkt_len = (u16)((pkt_len + 8) & ~3u);
	if (pkt_len < 32 || pkt_len > sizeof(rx_buf))
		return -2;
	for (i = 0; i < (int)sizeof(tx_buf); i++)
		tx_buf[i] = 0;
	spi_xfer_raw(tx_buf, rx_buf, pkt_len, 8);
	msg = rx_buf + 5;
	if (pkt_len < 5 + Z2_NFINGER_OFF + 1)
		return -3;
	nf = msg[Z2_NFINGER_OFF];
	if (nf == 0 || nf > 10)
		return 0;
	{
		const u8 *fp = msg + Z2_FINGER_OFF;
		u8 st = fp[1];

		if (st != Z2_TOUCH_START && st != Z2_TOUCH_MOVED)
			return 0;
		ax = (s16)(fp[4] | (fp[5] << 8));
		ay = (s16)(fp[6] | (fp[7] << 8));
	}
	{
		int x = ax, y = ay;

		if (x < 0)
			x = -x;
		if (y < 0)
			y = -y;
		if (x > 20000 || y > 20000) {
			x = (x * FB_W) / 65535;
			y = (y * FB_H) / 65535;
		} else if (x > FB_W || y > FB_H) {
			x = (x * FB_W) / 4096;
			y = (y * FB_H) / 4096;
		}
		if (x >= FB_W)
			x = FB_W - 1;
		if (y >= FB_H)
			y = FB_H - 1;
		if (x < 0)
			x = 0;
		if (y < 0)
			y = 0;
		cur_x = x;
		cur_y = y;
		have_touch = 1;
	}
	return 1;
}

static void ui_redraw(u32 bar)
{
	u32 stat;

	fb_fill(0, 0, FB_W, 28, bar);
	fb_fill(0, 32, FB_W, 500, BG);
	ui_line(36, "attn:", attn_lo, FG);
	ui_line(56, " ok:", frames_ok, HIT);
	ui_line(76, "fail:", frames_fail, MISS);
	stat = (g_phase << 24) | (g_boot_attn << 16) | (g_fw_ok << 8) | g_has_fw;
	ui_line(96, "stat:", stat, ACC);
	ui_line(116, " fw:", g_fw_size, WARN);
	ui_line(136, "err:", g_fw_err, g_fw_err ? MISS : DIM);
	ui_line(156, "gate:", g_gate_ok, (g_gate_ok & 0x5fu) == 0x5fu ? HIT : WARN);
	ui_line(176, "ibc:", g_ibec_st, (g_ibec_st & 0x1BEC001Fu) == 0x1BEC001Fu ? HIT : WARN);
	ui_line(196, "pmu:", g_pmu_st, g_pmu_st == 3u ? HIT : WARN);
	ui_line(216, "i2c:", g_i2c_st, (g_i2c_st & 0x300u) ? HIT : WARN);
	ui_line(236, "g68:", g_gate68_raw, (g_gate68_raw & 0xffu) == 0xffu ? HIT : WARN);
	ui_line(256, "rst:", g_rst_st, g_rst_st & 8u ? HIT : WARN);
	ui_line(276, "spi:", g_spi_rb, g_spi_rb ? HIT : WARN);
	ui_line(296, "base:", g_spi_base, g_spi_base == SPI1_BASE ? HIT : WARN);
	ui_line(316, "c04:", g_clk304, (g_clk304 & 0x180u) == 0x180u ? HIT : WARN);
	ui_line(336, "c07:", g_clk307, (g_clk307 & 0x180u) == 0x180u ? HIT : WARN);
	ui_line(356, "cpw:", g_clkpwm, (g_clkpwm & 0x180u) == 0x180u ? HIT : WARN);
	ui_line(376, "pwm:", g_pwm_peek, g_pwm_peek ? HIT : WARN);
	ui_line(396, "urt:", g_uart_peek, g_uart_peek ? HIT : WARN);
	ui_line(416, "ic0:", g_i2c_peek, g_i2c_peek ? HIT : WARN);
	ui_line(436, "g22:", gpio22_raw, WARN);
	ui_line(456, "rx0:", last_rx0, last_rx0 == Z2_REPLY_IRQ ? HIT : MISS);
	ui_line(476, "poll:", poll_n, DIM);
	if (have_touch)
		draw_cursor(cur_x, cur_y, CURSOR);
	v7_flush();
}

int main(void)
{
	u32 last_x = 0, last_y = 0;
	volatile u32 *fw = (volatile u32 *)FW_BASE;

	wdt_kill();
	if (!(read_sctlr() & 1u))
		g_rst_st |= 0x20u;
	fb_fill(0, 0, FB_W, FB_H, BG);
	gpio_set_out(GPIO_CS, 1);
	wr32(gpio, GPIO_RESET * 4, GPIO_RST_CFG);
	parity = 0;
	g_has_fw = (fw[0] == Z2FW_MAGIC && fw[1] == 1u) ? 1u : 0u;
	g_fw_size = 0;
	if (g_has_fw) {
		u32 idx = 8, end = 8;

		while (idx + 8 <= 512u * 1024u) {
			u32 cmd = le32r((const u8 *)fw + idx);

			idx += 4;
			if (cmd == LOAD_INIT || cmd == LOAD_BLOB) {
				u32 size;

				if (idx + 4 > 512u * 1024u)
					break;
				size = le32r((const u8 *)fw + idx);
				idx += 4;
				if (size == 0 || size > 256u * 1024u || idx + size > 512u * 1024u)
					break;
				idx += size;
				end = idx;
			} else if (cmd == LOAD_CAL) {
				idx += 4;
				end = idx;
			} else {
				break;
			}
			idx = (idx + 3u) & ~3u;
		}
		if (end > 8)
			g_fw_size = end;
	}
	ui_redraw(DIM);
	mt_power_up();
	mt_reset_and_boot();
	g_phase = 3;
	ui_redraw(g_fw_ok ? ACC : WARN);

	for (;;) {
		u32 attn = gpio_get(GPIO_ATTN);

		gpio22_raw = rd32(gpio, GPIO_ATTN * 4);
		spi_ch = rd32(g_spi, SPI_CH_CFG);
		spi_st = rd32(g_spi, SPI_STATUS);
		poll_n++;

		if (attn == 0 || (poll_n & 0xfffu) == 0) {
			if (attn == 0)
				attn_lo++;
			if (z2_read_packet() > 0) {
				frames_ok++;
				if ((u32)cur_x != last_x || (u32)cur_y != last_y) {
					fb_fill(0, 220, FB_W, FB_H - 220, BG);
					last_x = cur_x;
					last_y = cur_y;
				}
				ui_redraw(HIT);
			} else {
				frames_fail++;
				ui_redraw(attn == 0 ? MISS : WARN);
			}
		} else if ((poll_n & 0x3fffu) == 0) {
			ui_redraw(DIM);
		}
		delay(500);
	}
	return 0;
}
