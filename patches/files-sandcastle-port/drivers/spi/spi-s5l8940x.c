/*
 * SPI controller driver for S5L8940X (Apple A5) SoCs
 *
 * Copyright (C) 2020 Corellium LLC
 *
 * Based on spi-mt7621.c:
 *   Copyright (C) 2011 Sergiy <piratfm@gmail.com>
 *   Copyright (C) 2011-2013 Gabor Juhos <juhosg@openwrt.org>
 *   Copyright (C) 2014-2015 Felix Fietkau <nbd@nbd.name>
 *   Copyright (C) 2007-2008 Marvell Ltd.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/reset.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/gpio/consumer.h>

#define REG_CLKCFG                      0x00
#define  REG_CLKCFG_ENABLE              0xD
#define REG_CONFIG                      0x04
#define  REG_CONFIG_AUTOTX              (1 << 0)
#define  REG_CONFIG_PIOEN               (1 << 5)
#define  REG_CONFIG_SIOEN               (1 << 6)
#define  REG_CONFIG_IE_RXRDY            (1 << 7)
#define  REG_CONFIG_IE_TXEMPTY          (1 << 8)
#define  REG_CONFIG_16BIT               (1 << 15)
#define  REG_CONFIG_32BIT               (1 << 16)
#define  REG_CONFIG_IE_COMPL            (1 << 21)
#define  REG_CONFIG_SET                 (0x0010401E)
#define REG_STATUS                      0x08
#define  REG_STATUS_RXRDY               (1 << 0)
#define  REG_STATUS_TXEMPTY             (1 << 1)
#define  REG_STATUS_TXFIFO_MASK         (31 << 6)
#define   REG_STATUS_TXFIFO_SHIFT       6
#define  REG_STATUS_RXFIFO_MASK         (31 << 11)
#define   REG_STATUS_RXFIFO_SHIFT       11
#define  REG_STATUS_COMPL               (1 << 22)
#define REG_PIN                         0x0C
#define  REG_PIN_CS                     (1 << 1)
#define REG_TXDATA                      0x10
#define REG_RXDATA                      0x20
#define REG_CLKDIV                      0x30
#define  REG_CLKDIV_MAX                 2047
#define REG_RXCNT                       0x34
#define REG_CLKIDLE                     0x38
#define REG_TXCNT                       0x4C

#define SPI_FIFO                        16
#define TIMEOUT_MS                      1000

struct hx_spi {
    struct spi_controller *master;
    void __iomem *base;
    unsigned int clkfreq;
    unsigned int speed;
    struct clk *clk;
    struct gpio_descs *csgpio;

    spinlock_t lock;
    const unsigned char *tx_buf;
    unsigned char *rx_buf;
    unsigned int tx_compl, rx_compl, len;
    struct completion done;
};

static inline struct hx_spi *spidev_to_hx_spi(struct spi_device *spi)
{
    return spi_controller_get_devdata(spi->controller);
}

static void hx_spi_continue_tx(struct hx_spi *spi, unsigned status)
{
    unsigned maxtx = SPI_FIFO - ((status & REG_STATUS_TXFIFO_MASK) >> REG_STATUS_TXFIFO_SHIFT);
    unsigned data;

    while(spi->tx_compl < spi->len && maxtx) {
        if(spi->tx_buf)
            data = spi->tx_buf[spi->tx_compl];
        else
            data = 0x00;
        writel(data, spi->base + REG_TXDATA);
        spi->tx_compl ++;
        maxtx --;
    }

    if(spi->tx_compl < spi->len)
        writel(REG_CONFIG_SET | REG_CONFIG_PIOEN | REG_CONFIG_IE_TXEMPTY | REG_CONFIG_IE_RXRDY, spi->base + REG_CONFIG);
    else
        writel(REG_CONFIG_SET | REG_CONFIG_PIOEN | REG_CONFIG_IE_RXRDY, spi->base + REG_CONFIG);
}

static int hx_spi_continue_rx(struct hx_spi *spi, unsigned status)
{
    unsigned maxrx = (status & REG_STATUS_RXFIFO_MASK) >> REG_STATUS_RXFIFO_SHIFT;
    unsigned data;

    while(spi->rx_compl < spi->len && maxrx) {
        data = readl(spi->base + REG_RXDATA);
        if(spi->rx_buf)
            spi->rx_buf[spi->rx_compl] = data;
        spi->rx_compl ++;
        maxrx --;
    }

    if(spi->rx_compl >= spi->len) {
        writel(REG_CONFIG_SET, spi->base + REG_CONFIG);
        writel(REG_STATUS_COMPL | REG_STATUS_TXEMPTY | REG_STATUS_RXRDY, spi->base + REG_STATUS);
        return 1;
    }
    return 0;
}

static irqreturn_t hx_spi_irq(int irq, void *dev_id)
{
    struct hx_spi *spi = dev_id;
    unsigned long flags;
    unsigned status;

    spin_lock_irqsave(&spi->lock, flags);

    status = readl(spi->base + REG_STATUS);
    writel(status, spi->base + REG_STATUS);

    hx_spi_continue_tx(spi, status);

    if(hx_spi_continue_rx(spi, status))
        complete(&spi->done);

    spin_unlock_irqrestore(&spi->lock, flags);

    return IRQ_HANDLED;
}

static void hx_spi_set_cs(struct spi_device *spid, int enable)
{
    struct hx_spi *spi = spidev_to_hx_spi(spid);
    int cs = spi_get_chipselect(spid, 0);

    if(!spi->csgpio || cs >= spi->csgpio->ndescs)
        return;

    gpiod_direction_output(spi->csgpio->desc[cs], !enable);
}

static int hx_spi_prepare(struct spi_device *spid, unsigned int speed)
{
    struct hx_spi *spi = spidev_to_hx_spi(spid);
    u32 rate;

    rate = DIV_ROUND_UP(spi->clkfreq, speed);
    if(rate > REG_CLKDIV_MAX + 1)
        return -EINVAL;
    if(rate < 2)
        rate = 2;

    /* Dump SPI regs BEFORE any of our writes — see if hardware is
     * responsive at all with whatever state iBoot left it in. */
    pr_err("SPI: spi->base virt=%p\n", spi->base);
    pr_err("SPI: probe reg dump (before clocks/wake):\n");
    pr_err("SPI:   0x00 CTRL   =0x%x\n", readl(spi->base + 0x00));
    pr_err("SPI:   0x04 SETUP  =0x%x\n", readl(spi->base + 0x04));
    pr_err("SPI:   0x08 STATUS =0x%x\n", readl(spi->base + 0x08));
    pr_err("SPI:   0x0C CS_REG =0x%x\n", readl(spi->base + 0x0C));
    pr_err("SPI:   0x30 CLKDIV =0x%x\n", readl(spi->base + 0x30));

    /* Independent ioremap on hardcoded 0x32100000 in case devm mapping is wrong. */
    {
        void __iomem *alt = ioremap(0x32100000, 0x1000);
        pr_err("SPI: alt ioremap 0x32100000 -> virt=%p\n", alt);
        if (alt) {
            pr_err("SPI:   alt 0x00 CTRL   =0x%x\n", readl(alt + 0x00));
            pr_err("SPI:   alt 0x04 SETUP  =0x%x\n", readl(alt + 0x04));
            pr_err("SPI:   alt 0x08 STATUS =0x%x\n", readl(alt + 0x08));
            writel(0xDEADBE00, alt + 0x30);
            udelay(10);
            pr_err("SPI: alt write-test CLKDIV=0xDEADBE00 readback=0x%x\n",
                   readl(alt + 0x30));
            iounmap(alt);
        }
    }

    /* Test other well-known peripheral addresses to confirm ioremap works. */
    {
        void __iomem *i2c = ioremap(0x33200000, 0x100);   /* I2C0 which works */
        void __iomem *pwm = ioremap(0x33F00000, 0x100);   /* PWM */
        if (i2c) {
            pr_err("SPI: I2C0 sanity 0x00=0x%x 0x04=0x%x\n",
                   readl(i2c + 0x00), readl(i2c + 0x04));
            iounmap(i2c);
        }
        if (pwm) {
            pr_err("SPI: PWM sanity 0x00=0x%x 0x04=0x%x\n",
                   readl(pwm + 0x00), readl(pwm + 0x04));
            iounmap(pwm);
        }
    }

    /* spi1_hw_reset from touch_cursor.c: toggle CH_SW_RST (bit 5) at 0x00 */
    {
        u32 v = readl(spi->base + 0x00);
        pr_err("SPI: hw_reset pre CTRL=0x%x\n", v);
        writel(v | (1u << 5), spi->base + 0x00);
        mdelay(2);
        writel(v & ~(1u << 5), spi->base + 0x00);
        mdelay(2);
        pr_err("SPI: hw_reset post CTRL=0x%x\n", readl(spi->base + 0x00));
    }

    /* Try writing a known value to CLKDIV (which has no side effects if
     * hardware is off) and reading it back. If write sticks -> hw alive. */
    writel(0xAA, spi->base + 0x30);
    udelay(10);
    pr_err("SPI: write-test CLKDIV=0xAA readback=0x%x\n", readl(spi->base + 0x30));

    /* XNU AppleS5L8940XIO::_initPMGRState replay from pongo/touch_cursor.c
     * pmgr_bootstrap(). MUST BE FIRST — before any other clock/gate writes.
     * Without this, most peripherals (SPI1, PWM, etc.) remain dead after iBoot
     * handoff — reads return 0x22, writes are dropped. */
    {
        void __iomem *pm = ioremap(0x3F100000, 0x2000);
        if (pm) {
            u32 v;
            pr_err("SPI: PMGR bootstrap (XNU _initPMGRState replay) - FIRST\n");
            v = readl(pm + 0x1180);
            pr_err("SPI:   0x1180 pre=0x%x\n", v);
            writel(v | 0x80000000u, pm + 0x1180);
            writel(0x7FFEu,       pm + 0x1200);
            writel(0x3FFF8001u,   pm + 0x1204);
            writel(0x0014000Fu,   pm + 0x1010);
            writel(0x0014000Fu,   pm + 0x1014);
            writel(0x0014000Fu,   pm + 0x1018);
            writel(0x00100000u,   pm + 0x107C);
            udelay(1000);
            pr_err("SPI:   0x1180 post=0x%x  0x1200=0x%x  0x1204=0x%x\n",
                   readl(pm + 0x1180), readl(pm + 0x1200), readl(pm + 0x1204));
            iounmap(pm);
        }
    }

    /* Full dump of PMGR gate table 0x1000..0x11FC to see which gates
     * iBoot left enabled (nonzero) and confirm ID mapping. Each entry
     * is 4 bytes; XNU id N is at 0x1000 + N*4, iBoot id N is at 0x1008 + N*4. */
    {
        void __iomem *pm3 = ioremap(0x3F100000, 0x2000);
        if (pm3) {
            u32 off;
            pr_err("SPI: PMGR gate table dump 0x1000..0x11FC (nonzero only):\n");
            for (off = 0x1000; off < 0x1200; off += 4) {
                u32 v = readl(pm3 + off);
                if (v)
                    pr_err("SPI:   PMGR+0x%03x = 0x%08x  (xnu_id=%u ibot_id=%d)\n",
                           off, v, (off - 0x1000) / 4,
                           (int)((off - 0x1008) / 4));
            }
            iounmap(pm3);
        }
    }

    /* Enable PMGR clock/gate for PWM (grape-clk source).
     * touch_cursor.c enables CLK 4 (PWM) and GATE 83 (PWM) alongside SPI1.
     * Without these the PWM controller at 0x33500300 stays dead (0x22). */
    {
        void __iomem *pm2 = ioremap(0x3F100000, 0x2000);
        if (pm2) {
            u32 v;
            /* CLK 4 (PWM) — pmgr_clk_addr(4): id>>5=0, id&7=4, idx=4, off=0x10 */
            v = readl(pm2 + 0x10);
            pr_err("SPI: CLK4(PWM) pre=0x%x\n", v);
            writel((v & ~0x180u) | 0x180u, pm2 + 0x10);
            udelay(100);
            pr_err("SPI: CLK4(PWM) post=0x%x\n", readl(pm2 + 0x10));

            /* GATE_XNU 83 @ 0x1000 + 83*4 = 0x114C */
            v = readl(pm2 + 0x114C);
            pr_err("SPI: GXNU83(PWM) pre=0x%x\n", v);
            writel((v & 0xFFFFFEF0u) | 0xFu, pm2 + 0x114C);
            udelay(100);
            pr_err("SPI: GXNU83(PWM) post=0x%x\n", readl(pm2 + 0x114C));

            /* GATE 83 @ 0x1008 + 83*4 = 0x1154 */
            v = readl(pm2 + 0x1154);
            pr_err("SPI: GATE83(PWM) pre=0x%x\n", v);
            writel(v | 0xFu, pm2 + 0x1154);
            udelay(100);
            pr_err("SPI: GATE83(PWM) post=0x%x\n", readl(pm2 + 0x1154));

            iounmap(pm2);
        }
    }

    /* mt_grape_clk_on replay from pongo/touch_cursor.c: configure PWM ch2
     * at 0x33500300 (ADT /arm-io/pwm) to generate 32768 Hz "grape-clk".
     * Our clk-s5l8940x-pmgr.c 'touch clock' driver interprets 0x33500300
     * as a single-register clock gate (bit 19 = enable), but it's actually
     * a Samsung PWM controller. Without proper PWM setup the SPI1 bus clock
     * source is missing and SPI1 MMIO reads back 0x22. */
    {
        void __iomem *pwm = ioremap(0x33500300, 0x100);
        if (pwm) {
            const u32 GRAPE_CH = 2;
            u32 tc = (GRAPE_CH == 0) ? 0 : (GRAPE_CH + 1);
            u32 tcnt_off = 0x0C + GRAPE_CH * 0x0C;
            u32 tcfg0, tcfg1, tcon, peek;

            pr_err("SPI: PWM grape-clk setup (mt_grape_clk_on replay)\n");
            pr_err("SPI:   PWM pre TCFG0=0x%x TCFG1=0x%x TCON=0x%x\n",
                   readl(pwm + 0x00), readl(pwm + 0x04), readl(pwm + 0x08));

            /* TCFG0: set prescaler1 to 0xff (bits 8-15) */
            tcfg0 = readl(pwm + 0x00);
            tcfg0 = (tcfg0 & 0x00FFFFFFu) | (0xFFu << 8);
            writel(tcfg0, pwm + 0x00);

            /* TCFG1: clear divider nibble for our channel (=> /2 by default) */
            tcfg1 = readl(pwm + 0x04);
            tcfg1 &= ~(0xFu << (GRAPE_CH * 4));
            writel(tcfg1, pwm + 0x04);

            /* TCNTB<ch> and TCMPB<ch> */
            writel(732u,     pwm + tcnt_off);        /* count */
            writel(732u / 2, pwm + tcnt_off + 4);    /* compare */

            /* TCON: pulse manual-update bit, then enable + auto-reload */
            tcon = readl(pwm + 0x08);
            tcon |= (1u << (tc * 4 + 1));
            writel(tcon, pwm + 0x08);
            tcon &= ~(1u << (tc * 4 + 1));
            tcon |= (1u << (tc * 4 + 0)) | (1u << (tc * 4 + 3));
            writel(tcon, pwm + 0x08);
            mdelay(5);

            peek = readl(pwm + 0x08);
            pr_err("SPI:   PWM post TCFG0=0x%x TCFG1=0x%x TCON=0x%x\n",
                   readl(pwm + 0x00), readl(pwm + 0x04), peek);
            if (peek == 0) {
                /* Try alternate bases if PWM_BLK didn't take */
                void __iomem *alt;
                alt = ioremap(0x33500000, 0x100);
                if (alt) {
                    pr_err("SPI:   PWM alt 0x33500000 TCON=0x%x\n",
                           readl(alt + 0x08));
                    iounmap(alt);
                }
                alt = ioremap(0x33500500, 0x100);
                if (alt) {
                    pr_err("SPI:   PWM alt 0x33500500 TCON=0x%x\n",
                           readl(alt + 0x08));
                    iounmap(alt);
                }
            }
            iounmap(pwm);
        }
    }

    /* Full SPI1 clock enable sequence from touch_cursor.c:
     *   CLK 304 @ PMGR+0x020 (SPI1A) : set CLK_EN_BITS 0x180
     *   CLK 307 @ PMGR+0x02C (SPI1B) : set CLK_EN_BITS 0x180
     *   GATE_XNU 68 @ PMGR+0x1110    : (v & ~0x10F0) | 0xF
     *   GATE 68 @ PMGR+0x1118        : v |= 0xF
     * Only enabling gate 68 is NOT sufficient — hardware bus returns
     * garbage (all regs = 0x22) until clocks 304 and 307 are running. */
    {
        void __iomem *pmgr = ioremap(0x3F100000, 0x2000);
        u32 v;
        if (pmgr) {
            /* CLK 304 (SPI1A) — offset 0x020 per touch_cursor formula */
            v = readl(pmgr + 0x020);
            pr_err("SPI: CLK304 pre=0x%x\n", v);
            writel((v & ~0x180u) | 0x180u, pmgr + 0x020);
            udelay(100);
            pr_err("SPI: CLK304 post=0x%x\n", readl(pmgr + 0x020));

            /* CLK 307 (SPI1B) — offset 0x02C */
            v = readl(pmgr + 0x02C);
            pr_err("SPI: CLK307 pre=0x%x\n", v);
            writel((v & ~0x180u) | 0x180u, pmgr + 0x02C);
            udelay(100);
            pr_err("SPI: CLK307 post=0x%x\n", readl(pmgr + 0x02C));

            /* GATE_XNU 68 @ PMGR+0x1000+68*4 = 0x1110 */
            v = readl(pmgr + 0x1110);
            pr_err("SPI: GXNU68 pre=0x%x\n", v);
            writel((v & 0xFFFFFEF0u) | 0xFu, pmgr + 0x1110);
            udelay(100);
            pr_err("SPI: GXNU68 post=0x%x\n", readl(pmgr + 0x1110));

            /* GATE 68 @ PMGR+0x1008+68*4 = 0x1118 */
            v = readl(pmgr + 0x1118);
            pr_err("SPI: GATE68 pre=0x%x\n", v);
            writel(v | 0xFu, pmgr + 0x1118);
            udelay(100);
            pr_err("SPI: GATE68 post=0x%x\n", readl(pmgr + 0x1118));

            iounmap(pmgr);
        } else {
            pr_err("SPI: failed to ioremap PMGR\n");
        }
    }

    /* Post-clock dump — did the clock enables help? */
    pr_err("SPI: post-clock reg dump:\n");
    pr_err("SPI:   0x00 CTRL   =0x%x\n", readl(spi->base + 0x00));
    pr_err("SPI:   0x04 SETUP  =0x%x\n", readl(spi->base + 0x04));
    pr_err("SPI:   0x08 STATUS =0x%x\n", readl(spi->base + 0x08));
    pr_err("SPI:   0x30 CLKDIV =0x%x\n", readl(spi->base + 0x30));
    writel(0x55, spi->base + 0x30);
    udelay(10);
    pr_err("SPI: post-clock write-test CLKDIV=0x55 readback=0x%x\n", readl(spi->base + 0x30));

    /* apple_spi_wake sequence from touch_cursor.c (A5 hardware) */
    writel(0xf, spi->base + REG_STATUS);                  /* clear status */
    writel(readl(spi->base + REG_CLKCFG) | 0xc,
           spi->base + REG_CLKCFG);                       /* CTRL |= 0xc */
    writel(4, spi->base + REG_CLKDIV);                    /* CLK divider */
    writel(6, spi->base + REG_PIN);                       /* CS_REG = 6 */
    writel(0x10618, spi->base + REG_CONFIG);              /* SETUP */
    writel(1, spi->base + REG_CLKCFG);                    /* CTRL = 1 (enable) */
    mdelay(5);
    (void)rate;

    spi->speed = speed;

    return 0;
}

static int hx_spi_transfer_one_message(struct spi_controller *master, struct spi_message *m)
{
    unsigned long timeout = msecs_to_jiffies(TIMEOUT_MS);
    struct hx_spi *spi = spi_controller_get_devdata(master);
    struct spi_device *spid = m->spi;
    unsigned int speed = spid->max_speed_hz;
    struct spi_transfer *t = NULL;
    int status = 0;
    unsigned long flags;

    list_for_each_entry(t, &m->transfers, transfer_list)
        if(t->speed_hz < speed)
            speed = t->speed_hz;

    if(hx_spi_prepare(spid, speed)) {
        status = -EIO;
        goto msg_done;
    }

    hx_spi_set_cs(spid, 1);

    m->actual_length = 0;
    list_for_each_entry(t, &m->transfers, transfer_list) {
        spin_lock_irqsave(&spi->lock, flags);

        reinit_completion(&spi->done);

        spi->len = t->len;
        spi->tx_compl = spi->rx_compl = 0;
        spi->tx_buf = t->tx_buf;
        spi->rx_buf = t->rx_buf;

        /* touch_cursor.c-style byte-by-byte xfer (A5 SPI hardware) —
         * do NOT touch REG_CONFIG here: apple_spi_wake set SETUP = 0x10618,
         * touch_cursor never rewrites SETUP during xfer. Overwriting it
         * with REG_CONFIG_SET|PIOEN (0x10403E) kills the transfer. */
        spin_unlock_irqrestore(&spi->lock, flags);
        timeout = 1;  /* assume success */
        {
            unsigned i;
            unsigned deadline;
            u32 st, st0, setup_pre;
            setup_pre = readl(spi->base + REG_CONFIG);
            st0 = readl(spi->base + REG_STATUS);
            pr_err("SPI: xfer start len=%u setup=0x%x status=0x%x cs_pin=0x%x ctrl=0x%x\n",
                   t->len, setup_pre, st0,
                   readl(spi->base + REG_PIN), readl(spi->base + REG_CLKCFG));
            for (i = 0; i < t->len; i++) {
                u32 txw = 0xff;
                if (spi->tx_buf)
                    txw = ((const u8 *)spi->tx_buf)[i];
                /* RXLIM = 1 (get one byte back) */
                writel(1, spi->base + 0x34);
                /* wait for idle */
                deadline = 100000;
                while (((st = readl(spi->base + REG_STATUS)) & 0x1f0u) == 0x100u) {
                    if (!deadline--) break;
                    udelay(1);
                }
                writel(txw, spi->base + REG_TXDATA);
                /* wait for RX ready (status bit range 0x3e00) */
                deadline = 100000;
                while (!((st = readl(spi->base + REG_STATUS)) & 0x3e00u)) {
                    if (!deadline--) {
                        u32 rx_val = readl(spi->base + REG_RXDATA);
                        u32 rxcnt = readl(spi->base + 0x34);
                        u32 txcnt = readl(spi->base + 0x4C);
                        u32 setup_now = readl(spi->base + REG_CONFIG);
                        pr_err("SPI: byte %u TX-wait fail, status=0x%x rx=0x%x rxcnt=0x%x txcnt=0x%x setup=0x%x\n",
                               i, st, rx_val, rxcnt, txcnt, setup_now);
                        timeout = 0;
                        goto xfer_done;
                    }
                    udelay(1);
                }
                st = readl(spi->base + REG_RXDATA);
                if (i < 4 || i == t->len - 1)
                    pr_err("SPI: byte %u OK tx=0x%x rx=0x%x\n", i, txw, st & 0xff);
                if (spi->rx_buf)
                    ((u8 *)spi->rx_buf)[i] = (u8)st;
                spi->tx_compl++;
                spi->rx_compl++;
            }
xfer_done: ;
        }

        spin_lock_irqsave(&spi->lock, flags);

        /* clear status flags but LEAVE SETUP (REG_CONFIG) alone */
        writel(REG_STATUS_COMPL | REG_STATUS_TXEMPTY | REG_STATUS_RXRDY, spi->base + REG_STATUS);

        if(timeout == 0) {
            dev_err(&spid->dev, "transfer timed out with %d/%d remaining.\n", spi->len - spi->tx_compl, spi->len - spi->rx_compl);
            status = -ETIMEDOUT;
        }

        m->actual_length += t->len;

        spin_unlock_irqrestore(&spi->lock, flags);

        if(status)
            break;
    }

    hx_spi_set_cs(spid, 0);

msg_done:
    m->status = status;
    spi_finalize_current_message(master);

    return 0;
}

static int hx_spi_setup(struct spi_device *spid)
{
    struct hx_spi *spi = spidev_to_hx_spi(spid);

    if(!spid->max_speed_hz || spid->max_speed_hz > (spi->clkfreq / 2))
        spid->max_speed_hz = spi->clkfreq / 2;

    if(spid->max_speed_hz < spi->clkfreq / (REG_CLKDIV_MAX + 1)) {
        dev_err(&spid->dev, "setup: requested speed is too low: %d Hz\n", spid->max_speed_hz);
        return -EINVAL;
    }

    return 0;
}

static const struct of_device_id hx_spi_match[] = {
    { .compatible = "apple,s5l8940x-spi" },
    {},
};
MODULE_DEVICE_TABLE(of, hx_spi_match);

static int hx_spi_probe(struct platform_device *pdev)
{
    struct spi_controller *master;
    struct hx_spi *spi;
    void __iomem *base;
    struct clk *clk;
    struct gpio_descs *csgpio = NULL;
    int ret, ncs, irq;

    base = devm_platform_ioremap_resource(pdev, 0);
    if(IS_ERR(base))
        return PTR_ERR(base);

    clk = devm_clk_get(&pdev->dev, NULL);
    if(IS_ERR(clk)) {
        dev_err(&pdev->dev, "unable to get clock: %ld.\n", PTR_ERR(clk));
        return PTR_ERR(clk);
    }

    ncs = gpiod_count(&pdev->dev, "cs");
    if(ncs > 0) {
        csgpio = devm_gpiod_get_array(&pdev->dev, "cs", 0);
        if(IS_ERR(csgpio)) {
            if(PTR_ERR(csgpio) != -EPROBE_DEFER)
                dev_err(&pdev->dev, "failed to get chip select gpios: %ld\n", PTR_ERR(csgpio));
            return PTR_ERR(csgpio);
        }
    } else
        ncs = 0;

    ret = clk_prepare_enable(clk);
    if(ret)
        return ret;

    master = spi_alloc_master(&pdev->dev, sizeof(*spi));
    if(!master) {
        dev_info(&pdev->dev, "master allocation failed.\n");
        return -ENOMEM;
    }

    master->mode_bits = SPI_LSB_FIRST;
    master->flags = 0;
    master->setup = hx_spi_setup;
    master->transfer_one_message = hx_spi_transfer_one_message;
    master->bits_per_word_mask = SPI_BPW_MASK(8);
    master->dev.of_node = pdev->dev.of_node;
    master->num_chipselect = ncs ? ncs : 1;

    dev_set_drvdata(&pdev->dev, master);

    spi = spi_controller_get_devdata(master);
    spi->base = base;
    spi->clk = clk;
    spi->master = master;
    spi->clkfreq = clk_get_rate(spi->clk);
    spi->csgpio = csgpio;

    dev_err(&pdev->dev, "S5L8940X SPI at %d MHz, %d chip select GPIO%s.\n", spi->clkfreq / 1000000, ncs, ncs == 1 ? "" : "s");

    spin_lock_init(&spi->lock);
    init_completion(&spi->done);

    irq = platform_get_irq(pdev, 0);
    if(irq < 0)
        return irq;

    ret = devm_request_irq(&pdev->dev, irq, hx_spi_irq, 0, dev_name(&pdev->dev), spi);
    if(ret < 0)
        return ret;

    return devm_spi_register_controller(&pdev->dev, master);
}

static void hx_spi_remove(struct platform_device *pdev)
{
    struct spi_controller *master;
    struct hx_spi *spi;

    master = dev_get_drvdata(&pdev->dev);
    spi = spi_controller_get_devdata(master);

    clk_disable_unprepare(spi->clk);
}

MODULE_ALIAS("platform:spi-s5l8940x");

static struct platform_driver hx_spi_driver = {
    .driver = {
        .name = "spi-s5l8940x",
        .of_match_table = hx_spi_match,
    },
    .probe = hx_spi_probe,
    .remove = hx_spi_remove,
};

module_platform_driver(hx_spi_driver);

MODULE_DESCRIPTION("S5L8940X SoC SPI driver");
MODULE_AUTHOR("Corellium LLC");
MODULE_LICENSE("GPL");
