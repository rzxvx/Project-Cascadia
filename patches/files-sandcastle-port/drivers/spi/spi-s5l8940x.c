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
    u32 clkdiv;
    bool hw_ready;
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

/*
 * Controller setup, done once.  The block is powered by its "clocks" entry --
 * SPI1's power-state switch in PMGR, index 58 in the ADT's device-clocks table
 * -- which the probe's clk_prepare_enable turns on.  Here we bring the SPI
 * controller itself up: clock config, divider, and pin/config defaults.
 *
 * CLKCFG (offset 0) must end at 0xd (REG_CLKCFG_ENABLE), the value XNU's
 * AppleSamsungSPIController writes.  The bug that made every transfer time out
 * was here: this used to set bits 2,3 with |0xc and then immediately overwrite
 * offset 0 with a plain 1, clearing them again -- so the prescaler never ran,
 * nothing shifted, and the RX FIFO stayed empty forever.
 *
 * This was also once the home of a bring-up lab of blind writes into the PMGR
 * gate table at the wrong indices, a PLL predivider, and a Samsung-style PWM
 * setup at 0x33500300 -- none of which ever made SPI1 answer.  The wrong PMGR
 * gate address was the real problem; see docs/research/p105-pmgr-gates.md.
 */
static void hx_spi_hw_init(struct hx_spi *spi)
{
    /* clear any latched status, arm the FIFO thresholds (XNU writes this exact
     * value to offset 8 at bring-up: COMPL w1c | the low status bits) */
    writel(REG_STATUS_COMPL | 0xf, spi->base + REG_STATUS);
    writel(spi->clkdiv, spi->base + REG_CLKDIV);
    writel(6, spi->base + REG_PIN);                           /* CS idle high */
    writel(REG_CONFIG_SET, spi->base + REG_CONFIG);           /* base config */
    writel(REG_CLKCFG_ENABLE, spi->base + REG_CLKCFG);        /* 0xd: run the clock */
    mdelay(5);
}

static int hx_spi_prepare(struct spi_device *spid, unsigned int speed)
{
    struct hx_spi *spi = spidev_to_hx_spi(spid);

    if(!spi->hw_ready) {
        hx_spi_hw_init(spi);
        spi->hw_ready = true;
        dev_info(&spi->master->dev, "controller up: CLKCFG %#x CONFIG %#x PIN %#x CLKDIV %u\n",
                 readl(spi->base + REG_CLKCFG), readl(spi->base + REG_CONFIG),
                 readl(spi->base + REG_PIN), readl(spi->base + REG_CLKDIV));
    }
    spi->speed = speed;
    return 0;
}

/*
 * Polled FIFO transfer, the S5L8940X (Samsung, spi-version 1) layout.
 *
 * The old byte-by-byte loop here came from touch_cursor.c, which drives a
 * *later* Apple SPI generation: its status masks (0x1f0, 0x3e00) are for a
 * different register.  On this controller the FIFO levels live at STATUS bits
 * [10:6] (TX) and [15:11] (RX) -- exactly the REG_STATUS_*FIFO_MASK values
 * below, and exactly what XNU's AppleSamsungSPIController::doTransfer uses.  A
 * PIO transfer here is: set the RX/TX packet counts, raise PIOEN, keep the TX
 * FIFO fed and drain the RX FIFO until every byte is back.
 *
 * We poll rather than take the completion interrupt: the AIC masks most lines
 * after its rearm (the PWM line had to be unmasked by hand), and a transfer
 * that blocks forever on an interrupt that never arrives is exactly the kind of
 * hang we are trying to avoid.  Transfers here are tens of bytes, microseconds
 * of busy-wait, so the completion IRQ (hx_spi_irq, still wired for later use)
 * is left disabled by never setting the IE bits.
 */
static void hx_spi_poll_tx(struct hx_spi *spi, u32 status)
{
    unsigned free = SPI_FIFO - ((status & REG_STATUS_TXFIFO_MASK) >> REG_STATUS_TXFIFO_SHIFT);

    while(spi->tx_compl < spi->len && free) {
        u32 data = spi->tx_buf ? spi->tx_buf[spi->tx_compl] : 0x00;
        writel(data, spi->base + REG_TXDATA);
        spi->tx_compl++;
        free--;
    }
}

static int hx_spi_poll_rx(struct hx_spi *spi, u32 status)
{
    unsigned level = (status & REG_STATUS_RXFIFO_MASK) >> REG_STATUS_RXFIFO_SHIFT;

    while(spi->rx_compl < spi->len && level) {
        u32 data = readl(spi->base + REG_RXDATA);
        if(spi->rx_buf)
            spi->rx_buf[spi->rx_compl] = data;
        spi->rx_compl++;
        level--;
    }
    return spi->rx_compl >= spi->len;
}

static int hx_spi_transfer_one_message(struct spi_controller *master, struct spi_message *m)
{
    struct hx_spi *spi = spi_controller_get_devdata(master);
    struct spi_device *spid = m->spi;
    unsigned int speed = spid->max_speed_hz;
    struct spi_transfer *t = NULL;
    int status = 0;

    list_for_each_entry(t, &m->transfers, transfer_list)
        if(t->speed_hz && t->speed_hz < speed)
            speed = t->speed_hz;

    if(hx_spi_prepare(spid, speed)) {
        status = -EIO;
        goto msg_done;
    }

    hx_spi_set_cs(spid, 1);

    m->actual_length = 0;
    list_for_each_entry(t, &m->transfers, transfer_list) {
        unsigned long deadline;

        spi->len = t->len;
        spi->tx_compl = spi->rx_compl = 0;
        spi->tx_buf = t->tx_buf;
        spi->rx_buf = t->rx_buf;

        if(!t->len)
            continue;

        /* Samsung layout: program the packet counts, then start PIO. */
        writel(t->len, spi->base + REG_RXCNT);
        writel(t->len, spi->base + REG_TXCNT);
        writel(REG_CONFIG_SET | REG_CONFIG_PIOEN, spi->base + REG_CONFIG);

        deadline = jiffies + msecs_to_jiffies(TIMEOUT_MS);
        for(;;) {
            u32 st = readl(spi->base + REG_STATUS);

            hx_spi_poll_tx(spi, st);
            if(hx_spi_poll_rx(spi, st))
                break;

            if(time_after(jiffies, deadline)) {
                dev_err(&spid->dev,
                        "transfer timed out: %u/%u tx, %u/%u rx, status=0x%x rxcnt=0x%x txcnt=0x%x config=0x%x\n",
                        spi->tx_compl, spi->len, spi->rx_compl, spi->len,
                        st, readl(spi->base + REG_RXCNT),
                        readl(spi->base + REG_TXCNT), readl(spi->base + REG_CONFIG));
                status = -ETIMEDOUT;
                break;
            }
            cpu_relax();
        }

        /* stop the channel and clear latched status */
        writel(REG_CONFIG_SET, spi->base + REG_CONFIG);
        writel(REG_STATUS_COMPL | REG_STATUS_TXEMPTY | REG_STATUS_RXRDY, spi->base + REG_STATUS);

        if(status)
            break;

        m->actual_length += t->len;
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
    /* touch_cursor.c used 4; the input clock is not known yet, so it stays
     * overridable from the device tree until someone measures it. */
    if(of_property_read_u32(pdev->dev.of_node, "apple,clkdiv", &spi->clkdiv))
        spi->clkdiv = 4;
    if(spi->clkdiv > REG_CLKDIV_MAX)
        spi->clkdiv = REG_CLKDIV_MAX;

    dev_info(&pdev->dev, "S5L8940X SPI, powered: CLKDIV %u reads back %#x, %d chip select GPIO%s.\n",
             spi->clkdiv, readl(base + REG_CLKDIV), ncs, ncs == 1 ? "" : "s");

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
