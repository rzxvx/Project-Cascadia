// SPDX-License-Identifier: GPL-2.0
/*
 * SPI controller of the older Apple SoCs: the ADT's "spi-1,samsung",
 * spi-version 1 (XNU's AppleSamsungSPIController). On the A13 (T8030) the
 * multitouch controller hangs off spi1.
 *
 * Polled PIO only. The register layout and the CLKCFG/STATUS handling come
 * from Corellium's Sandcastle driver for the A10, as fixed for the A5 in
 * Cascadia (ipad-mini-linux, spi-s5l8940x.c):
 *
 * Copyright (C) 2020 Corellium LLC
 *
 * Differences from that driver: chip select through the SPI core (cs-gpios),
 * power through the PMGR power domain, no clock (the divider is set
 * directly), and the divider and CONFIG word can be changed at run time
 * (/sys/module/spi_apple_s5l/parameters) while the controller is brought up.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>

#define REG_CLKCFG		0x00
#define  REG_CLKCFG_ENABLE	0xd	/* run the clock, reset both FIFOs */
#define REG_CONFIG		0x04
#define  REG_CONFIG_PIOEN	(1 << 5)
#define  REG_CONFIG_DEFAULT	0x4018	/* 8-bit words, XNU's base word on the A5 */
#define REG_STATUS		0x08
#define  REG_STATUS_RXRDY	(1 << 0)
#define  REG_STATUS_TXEMPTY	(1 << 1)
#define  REG_STATUS_TXFIFO	GENMASK(10, 6)
#define  REG_STATUS_RXFIFO	GENMASK(15, 11)
#define  REG_STATUS_COMPL	(1 << 22)
#define REG_PIN			0x0c
#define  REG_PIN_IDLE		0x6	/* CS idle high */
#define REG_TXDATA		0x10
#define REG_RXDATA		0x20
#define REG_CLKDIV		0x30
#define  REG_CLKDIV_MAX		2047
#define REG_RXCNT		0x34
#define REG_TXCNT		0x4c

#define SPI_FIFO		16
#define TIMEOUT_MS		1000

static uint clkdiv;
module_param(clkdiv, uint, 0644);
MODULE_PARM_DESC(clkdiv, "CLKDIV register value; 0: from the transfer's speed (input clock 24 MHz)");

static uint config = REG_CONFIG_DEFAULT;
module_param(config, uint, 0644);
MODULE_PARM_DESC(config, "CONFIG word without PIOEN (word size, mode bits)");

static uint debug;
module_param(debug, uint, 0644);
MODULE_PARM_DESC(debug, "log this many transfers (head of TX and RX)");

struct s5l_spi {
	void __iomem *base;
	struct device *dev;
	unsigned long rate;	/* input clock; CLKDIV divides it (measured: 16 -> ~1.4 MHz) */
};

static u32 s5l_spi_div(struct s5l_spi *spi, u32 speed_hz)
{
	u32 div = clkdiv;

	if (!div)
		div = speed_hz ? DIV_ROUND_UP(spi->rate, speed_hz) : 16;
	return clamp(div, 1u, (u32)REG_CLKDIV_MAX);
}

static void s5l_spi_hw_init(struct s5l_spi *spi)
{
	writel(REG_STATUS_COMPL | 0xf, spi->base + REG_STATUS);
	writel(s5l_spi_div(spi, 0), spi->base + REG_CLKDIV);
	writel(REG_PIN_IDLE, spi->base + REG_PIN);
	writel(config, spi->base + REG_CONFIG);
	writel(REG_CLKCFG_ENABLE, spi->base + REG_CLKCFG);
}

static int s5l_spi_transfer_one(struct spi_controller *ctlr,
				struct spi_device *sdev, struct spi_transfer *t)
{
	struct s5l_spi *spi = spi_controller_get_devdata(ctlr);
	const u8 *tx = t->tx_buf;
	u8 *rx = t->rx_buf;
	unsigned int len = t->len, txd = 0, rxd = 0;
	unsigned long deadline;
	u32 st = 0;
	int ret = 0;

	if (!len)
		return 0;

	/*
	 * XNU's order: CLKCFG first, every transfer (it also resets the FIFOs),
	 * then clear status, set the counts and start PIO.
	 */
	writel(s5l_spi_div(spi, t->speed_hz), spi->base + REG_CLKDIV);
	writel(REG_CLKCFG_ENABLE, spi->base + REG_CLKCFG);
	writel(REG_STATUS_COMPL | 0xf, spi->base + REG_STATUS);
	writel(len, spi->base + REG_RXCNT);
	writel(len, spi->base + REG_TXCNT);
	writel(config | REG_CONFIG_PIOEN, spi->base + REG_CONFIG);

	deadline = jiffies + msecs_to_jiffies(TIMEOUT_MS);
	while (rxd < len) {
		unsigned int n;

		st = readl(spi->base + REG_STATUS);

		n = SPI_FIFO - FIELD_GET(REG_STATUS_TXFIFO, st);
		while (txd < len && n--) {
			writel(tx ? tx[txd] : 0, spi->base + REG_TXDATA);
			txd++;
		}

		n = FIELD_GET(REG_STATUS_RXFIFO, st);
		while (rxd < len && n--) {
			u32 v = readl(spi->base + REG_RXDATA);

			if (rx)
				rx[rxd] = v;
			rxd++;
		}

		if (time_after(jiffies, deadline)) {
			dev_err(spi->dev,
				"timeout: tx %u/%u rx %u/%u status %#x rxcnt %#x txcnt %#x config %#x clkcfg %#x\n",
				txd, len, rxd, len, st,
				readl(spi->base + REG_RXCNT), readl(spi->base + REG_TXCNT),
				readl(spi->base + REG_CONFIG), readl(spi->base + REG_CLKCFG));
			ret = -ETIMEDOUT;
			break;
		}
		cpu_relax();
	}

	if (debug) {
		debug--;
		dev_info(spi->dev, "xfer %u: tx %*ph rx %*ph status %#x\n", len,
			 tx ? (int)min(len, 8u) : 0, tx, rx ? (int)min(len, 8u) : 0, rx,
			 readl(spi->base + REG_STATUS));
	}

	writel(config, spi->base + REG_CONFIG);
	writel(REG_STATUS_COMPL | REG_STATUS_TXEMPTY | REG_STATUS_RXRDY,
	       spi->base + REG_STATUS);

	return ret;
}

static int s5l_spi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct spi_controller *ctlr;
	struct s5l_spi *spi;

	ctlr = devm_spi_alloc_host(dev, sizeof(*spi));
	if (!ctlr)
		return -ENOMEM;

	spi = spi_controller_get_devdata(ctlr);
	spi->dev = dev;
	spi->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(spi->base))
		return PTR_ERR(spi->base);

	{
		struct clk *clk = devm_clk_get_optional_enabled(dev, NULL);

		if (IS_ERR(clk))
			return PTR_ERR(clk);
		spi->rate = clk ? clk_get_rate(clk) : 0;
		if (!spi->rate)
			spi->rate = 24000000;
	}

	ctlr->dev.of_node = dev->of_node;
	ctlr->bus_num = -1;
	ctlr->num_chipselect = 1;
	ctlr->use_gpio_descriptors = true;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	ctlr->max_speed_hz = spi->rate;
	ctlr->min_speed_hz = DIV_ROUND_UP(spi->rate, REG_CLKDIV_MAX);
	ctlr->transfer_one = s5l_spi_transfer_one;

	s5l_spi_hw_init(spi);
	dev_info(dev, "up: CLKCFG %#x CONFIG %#x STATUS %#x PIN %#x CLKDIV %u\n",
		 readl(spi->base + REG_CLKCFG), readl(spi->base + REG_CONFIG),
		 readl(spi->base + REG_STATUS), readl(spi->base + REG_PIN),
		 readl(spi->base + REG_CLKDIV));

	return devm_spi_register_controller(dev, ctlr);
}

static const struct of_device_id s5l_spi_of_match[] = {
	{ .compatible = "apple,s5l-spi" },
	{}
};
MODULE_DEVICE_TABLE(of, s5l_spi_of_match);

static struct platform_driver s5l_spi_driver = {
	.probe = s5l_spi_probe,
	.driver = {
		.name = "spi-apple-s5l",
		.of_match_table = s5l_spi_of_match,
	},
};
module_platform_driver(s5l_spi_driver);

MODULE_DESCRIPTION("Apple S5L-style (\"spi-1,samsung\") SPI controller");
MODULE_LICENSE("GPL");
