// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple S5L89xx SDIO host -- the iPad 2's Wi-Fi bus.
 *
 * The ADT's arm-io/sdio ("sdio,s5l8940x", "sdio,s5l8920x"): an SD host
 * controller at 0x30000000, IRQ 39, whose only device is the BCM4329.  iOS
 * never touches it from the CPU: AppleIOPSDIO hands every transfer to the
 * IOP, Apple's I/O coprocessor, whose firmware has the SDHC driver
 * ("sdiodrv_resetSDHC"), and the data moves through CDMA channel 3 to the
 * controller's +0x20 -- SDHCI's buffer data port.  So the register file is
 * the standard one, and Linux drives it directly with the SDHCI core.
 *
 * What is Apple's here:
 *
 *   - power: PMGR's SDIO-WIFI power state (device-clocks id 56, register
 *     0x3f1010b8), which iBoot leaves off; the first clock in the DT node.
 *   - DMA: the controller's own SDMA/ADMA are not what iOS uses, so they are
 *     not trusted: PIO through the buffer data port, as the IOP's CDMA does
 *     it, unless dma=1.
 *   - the base clock: from the capabilities register when it gives one, else
 *     the DT's clock-frequency (or base_hz=).
 *   - a 2.00 host whatever its version register says, and the clock set the
 *     IOP's way (sdhci_apple_s5l_set_clock, sdhci_apple_s5l_start);
 *   - a data buffer of two banks that no reset clears: re-aligned after
 *     every data error (sdhci_apple_s5l_resync);
 *   - and for the card behind it, Apple's BCM4329 module, whose CIS has no
 *     FUNCE: a default speed and block-size handling (init_card).
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mmc/card.h>
#include <linux/mmc/sdio.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include "sdhci-pltfm.h"

static bool use_dma;
module_param_named(dma, use_dma, bool, 0444);
MODULE_PARM_DESC(dma, "let the SDHCI core use the controller's own DMA (default: PIO)");

static unsigned int base_hz;
module_param(base_hz, uint, 0444);
MODULE_PARM_DESC(base_hz, "SD base clock in Hz, when the capabilities do not say");

static bool trace;
module_param(trace, bool, 0644);
MODULE_PARM_DESC(trace, "log every clock change (bring-up)");

/* bring-up: power the block and stop there, no MMC host -- so the
 * controller can be driven by hand (tools/k93-sdio-probe.c) without the MMC
 * core retrying under it; writing 1 to "release" then hands it to the MMC
 * core, with whatever tracing is wanted already switched on */
static bool hold;
module_param(hold, bool, 0444);
MODULE_PARM_DESC(hold, "power the controller and register no MMC host (bring-up)");

/* bring-up knobs, read when they are used: set them while the controller is
 * held, then release it (or unbind/bind 30000000.sdio to start over) */
static unsigned int max_blk;
module_param(max_blk, uint, 0644);
MODULE_PARM_DESC(max_blk, "blocks per CMD53 at most, from the next card init (0: the host's own limit)");

static unsigned int cap_hz;
module_param(cap_hz, uint, 0644);
MODULE_PARM_DESC(cap_hz, "SD clock at most this many Hz (0: as the core asks)");

static unsigned int irq_log;
module_param(irq_log, uint, 0644);
MODULE_PARM_DESC(irq_log, "log this many more data interrupts (status, present state, blocks left), data errors included");

static struct platform_device *held;
static int sdhci_apple_s5l_start(struct platform_device *pdev);

static int release_set(const char *val, const struct kernel_param *kp)
{
	struct platform_device *pdev = held;
	bool go;
	int ret = kstrtobool(val, &go);

	if (ret || !go)
		return ret;
	if (!pdev)
		return -ENODEV;
	held = NULL;
	return sdhci_apple_s5l_start(pdev);
}

static const struct kernel_param_ops release_ops = {
	.set = release_set,
};
module_param_cb(release, &release_ops, NULL, 0200);
MODULE_PARM_DESC(release, "write 1: hand a held controller to the MMC core");

struct sdhci_apple_s5l {
	struct clk *gate;
	unsigned int base_hz;
	u32 cis;		/* the common CIS's address */
	u32 known_cis;		/* its first word, and CCCR 0x08..0x0b */
	u32 known_cccr;		/* (0 until read: CCCR 0x08 is never 0 here) */
	unsigned int realigned;
};

static unsigned int sdhci_apple_s5l_get_max_clock(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_apple_s5l *priv = sdhci_pltfm_priv(pltfm_host);

	return priv->base_hz;
}

/*
 * The clock, the IOP firmware's way (EmbeddedIOP s5l8940xiop, 12H321: the
 * SD clock set at 0xa230, its helpers 0xa720..0xa7b8), which differs from
 * sdhci_set_clock() in two ways this controller notices:
 *
 *   - every write to clock control is a read-modify-write: what reads back,
 *     the internal-clock-stable bit (1) included, goes back with the bits
 *     being changed.  sdhci_set_clock() writes divider | INT_EN | CARD_EN
 *     with bit 1 clear, and on this block the clock then read back 0 and
 *     every command timed out with no interrupt (first boot, 2026-11-09);
 *   - the divisor is a power of two, the SDHCI v2 encoding (field = div/2),
 *     although the version register says 3.00.
 */
static void sdhci_apple_s5l_set_clock(struct sdhci_host *host, unsigned int clock)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_apple_s5l *priv = sdhci_pltfm_priv(pltfm_host);
	unsigned int div = 1, tries;
	u16 clk;

	host->mmc->actual_clock = 0;
	if (cap_hz && clock > cap_hz)
		clock = cap_hz;
	if (trace)
		dev_info(mmc_dev(host->mmc), "set_clock %u Hz (clock control %#06x)\n",
			 clock, sdhci_readw(host, SDHCI_CLOCK_CONTROL));

	/* the SD clock off first, the rest as it reads */
	clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
	sdhci_writew(host, clk & ~SDHCI_CLOCK_CARD_EN, SDHCI_CLOCK_CONTROL);
	if (!clock) {
		sdhci_writew(host, 0, SDHCI_CLOCK_CONTROL);
		return;
	}

	while (div < 256 && priv->base_hz / div > clock)
		div <<= 1;

	/* the divider and the internal clock, keeping the low byte as read */
	clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL) & 0x00ff & ~SDHCI_CLOCK_CARD_EN;
	clk |= (div >> 1) << SDHCI_DIVIDER_SHIFT | SDHCI_CLOCK_INT_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

	for (tries = 0; tries < 150; tries++) {
		clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
		if (clk & SDHCI_CLOCK_INT_STABLE)
			break;
		udelay(100);
	}
	if (!(clk & SDHCI_CLOCK_INT_STABLE)) {
		dev_err(mmc_dev(host->mmc), "internal clock never stable (clock control %#06x)\n", clk);
		return;
	}

	/* and the card clock: what reads back, bit 1 set, plus CARD_EN */
	sdhci_writew(host, clk | SDHCI_CLOCK_CARD_EN, SDHCI_CLOCK_CONTROL);
	host->mmc->actual_clock = priv->base_hz / div;
	if (trace)
		dev_info(mmc_dev(host->mmc), "  -> %u Hz (divisor %u), clock control %#06x\n",
			 host->mmc->actual_clock, div, sdhci_readw(host, SDHCI_CLOCK_CONTROL));
}

/*
 * The card's transfer speed when its CIS gives none.  Apple's BCM4329 module
 * carries its own common CIS: MANFID (02d0:4329), VERS_1 ("s=C0 P=K94 m=1.0
 * V=a"), one vendor tuple (0x80, the module's serial) and END -- no FUNCE
 * (traced on the iPad 2, 2026-11-09).  The core takes the speed from FUNCE
 * alone (CCCR rev 1.10: the high-speed bit is not read), so cis.max_dtr stays
 * 0 and right after the CIS it sets the clock to 0 Hz; the next CMD52 (CCCR
 * 0x07, for the bus width) then times out and the card is dropped.
 *
 * init_card runs after CMD5, before the CIS is read, so what is set here is a
 * default that a FUNCE tuple, on a card that has one, still overrides.  The
 * host's f_max is the DT's max-frequency.  (iOS runs this card at 51.3 MHz in
 * high-speed mode, ADT sdio.card.clock-rate / speed-mode 17; not here yet.)
 *
 * The functions have no CIS at all: FBR 0x109, 0x209 and 0x309 all point at
 * 0x10a5, the common CIS's END byte.  So no function has a max_blksize, and
 * the core's byte-mode transfers come out 0 bytes long (WARN_ON(blksz == 0)
 * in mmc_io_rw_extended; brcmfmac read its chip signature as 0xffffffff).
 * BLKSZ_FOR_BYTE_MODE bounds them by the block size in use instead, which
 * brcmfmac sets for both functions before its first transfer (64 and 128).
 */
static void sdhci_apple_s5l_init_card(struct mmc_host *mmc, struct mmc_card *card)
{
	if (!card->cis.max_dtr) {
		card->cis.max_dtr = mmc->f_max;
		dev_info(mmc_dev(mmc), "card speed %u Hz unless its CIS says otherwise\n",
			 card->cis.max_dtr);
	}
	card->quirks |= MMC_QUIRK_BLKSZ_FOR_BYTE_MODE;
	if (max_blk) {
		mmc->max_blk_count = max_blk;
		dev_info(mmc_dev(mmc), "at most %u blocks per transfer\n", max_blk);
	}
}

/* bring-up: what the data interrupts look like, before the core acts on them */
static u32 sdhci_apple_s5l_irq(struct sdhci_host *host, u32 intmask)
{
	if (irq_log && (intmask & SDHCI_INT_DATA_MASK)) {
		irq_log--;
		dev_info(mmc_dev(host->mmc),
			 "irq %08x present %08x blocks left %u (size %#06x, mode %#06x)\n",
			 intmask, sdhci_readl(host, SDHCI_PRESENT_STATE),
			 sdhci_readw(host, SDHCI_BLOCK_COUNT),
			 sdhci_readw(host, SDHCI_BLOCK_SIZE),
			 sdhci_readw(host, SDHCI_TRANSFER_MODE));
	}
	return intmask;
}

/*
 * The data buffer has two banks, and nothing resets them: not the DAT-line
 * reset, not a reset-all, not even switching the block's PMGR power state
 * off and on (tools/k93-sdio-probe.c -e and -L, on the iPad 2, 2026-11-09).
 * A read whose data reached the buffer and was never read out -- every read
 * that ends in a data error -- leaves the banks one apart, and from then on
 * every read returns the data of the one before: a register one read late,
 * for good, until the next such error swaps them back.  Reading the data
 * port outside a transfer does not move them.
 *
 * brcmfmac meets it on its very first pass: this BCM4329 answers its read of
 * chipcommon's capabilities_ext (0x180000ac, which older chipcommons lack)
 * with a bad end bit.  Every register read after it came back shifted, the
 * read-modify-write of the PMU control register that followed wrote the PMU
 * capabilities into it, and the chip then took two blocks of its firmware
 * and hung its backplane with DAT0 held low.
 *
 * So after every DAT-line reset with an SDIO card on the bus, the driver
 * checks and puts it right, with the controller to itself (the SDHCI core
 * resets from inside its request completion, host lock held): two 4-byte
 * CMD53 reads of words that never change, the common CIS's first and CCCR
 * 0x08..0x0b.  In step, the second returns CCCR 0x08..0x0b; one late, it
 * returns the CIS word -- and then one more read is left unread across a
 * reset, which swaps the banks back, and the check runs again.
 */
static bool s5l_wait_idle(struct sdhci_host *host, u32 mask)
{
	int i;

	for (i = 0; i < 2000; i++) {
		if (!(sdhci_readl(host, SDHCI_PRESENT_STATE) & mask))
			return true;
		udelay(10);
	}
	return false;
}

static int s5l_raw_cmd52_read(struct sdhci_host *host, u32 addr, u8 *val)
{
	u32 st = 0;
	int i;

	if (!s5l_wait_idle(host, SDHCI_CMD_INHIBIT))
		return -EBUSY;
	sdhci_writel(host, ~0, SDHCI_INT_STATUS);
	sdhci_writel(host, (addr & 0x1ffff) << 9, SDHCI_ARGUMENT);
	sdhci_writew(host, SDHCI_MAKE_CMD(SD_IO_RW_DIRECT, (SDHCI_CMD_RESP_SHORT |
					  SDHCI_CMD_CRC | SDHCI_CMD_INDEX)), SDHCI_COMMAND);
	for (i = 0; i < 1000; i++) {
		st = sdhci_readl(host, SDHCI_INT_STATUS);
		if (st & (SDHCI_INT_RESPONSE | SDHCI_INT_ERROR))
			break;
		udelay(5);
	}
	sdhci_writel(host, ~0, SDHCI_INT_STATUS);
	if (!(st & SDHCI_INT_RESPONSE) || (st & SDHCI_INT_ERROR))
		return -EIO;
	*val = sdhci_readl(host, SDHCI_RESPONSE) & 0xff;
	return 0;
}

/* a 4-byte CMD53 read on function 0; with no val, the data stays unread */
static int s5l_raw_read4(struct sdhci_host *host, u32 addr, u32 *val)
{
	u32 st = 0;
	int i;

	if (!s5l_wait_idle(host, SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT))
		return -EBUSY;
	sdhci_writel(host, ~0, SDHCI_INT_STATUS);
	sdhci_writew(host, SDHCI_MAKE_BLKSZ(host->sdma_boundary, 4), SDHCI_BLOCK_SIZE);
	sdhci_writew(host, 1, SDHCI_BLOCK_COUNT);
	sdhci_writel(host, 1u << 26 | (addr & 0x1ffff) << 9 | 4, SDHCI_ARGUMENT);
	sdhci_writew(host, SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN, SDHCI_TRANSFER_MODE);
	sdhci_writew(host, SDHCI_MAKE_CMD(SD_IO_RW_EXTENDED, (SDHCI_CMD_RESP_SHORT |
					  SDHCI_CMD_CRC | SDHCI_CMD_INDEX | SDHCI_CMD_DATA)),
		     SDHCI_COMMAND);
	for (i = 0; i < 2000; i++) {
		if (sdhci_readl(host, SDHCI_PRESENT_STATE) & SDHCI_DATA_AVAILABLE)
			break;
		if (sdhci_readl(host, SDHCI_INT_STATUS) & SDHCI_INT_ERROR)
			return -EIO;
		udelay(5);
	}
	if (!(sdhci_readl(host, SDHCI_PRESENT_STATE) & SDHCI_DATA_AVAILABLE))
		return -ETIMEDOUT;
	if (!val)
		return 0;
	*val = sdhci_readl(host, SDHCI_BUFFER);
	for (i = 0; i < 2000; i++) {
		st = sdhci_readl(host, SDHCI_INT_STATUS);
		if (st & (SDHCI_INT_DATA_END | SDHCI_INT_ERROR))
			break;
		udelay(5);
	}
	sdhci_writel(host, ~0, SDHCI_INT_STATUS);
	return (st & SDHCI_INT_DATA_END) && !(st & SDHCI_INT_ERROR) ? 0 : -EIO;
}

static void sdhci_apple_s5l_resync(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_apple_s5l *priv = sdhci_pltfm_priv(pltfm_host);
	struct mmc_card *card = host->mmc->card;
	u32 sig, a, b;
	int i, tries;
	u8 v;

	if (!card || !mmc_card_sdio(card))
		return;
	sig = sdhci_readl(host, SDHCI_SIGNAL_ENABLE);
	sdhci_writel(host, 0, SDHCI_SIGNAL_ENABLE);

	if (!priv->known_cccr) {
		/* CMD52 does not go through the buffer: the words, for sure */
		for (a = b = 0, i = 0; i < 4; i++) {
			if (s5l_raw_cmd52_read(host, SDIO_CCCR_CAPS + i, &v))
				goto out;
			b |= (u32)v << (8 * i);
		}
		priv->cis = b >> 8;		/* CCCR 0x09..0x0b */
		for (i = 0; i < 4; i++) {
			if (s5l_raw_cmd52_read(host, priv->cis + i, &v))
				goto out;
			a |= (u32)v << (8 * i);
		}
		if (a == b) {
			dev_warn(mmc_dev(host->mmc), "buffer check impossible: CIS and CCCR read alike\n");
			goto out;
		}
		priv->known_cis = a;
		priv->known_cccr = b;
	}

	for (tries = 0; tries < 4; tries++) {
		if (s5l_raw_read4(host, priv->cis, &a) || s5l_raw_read4(host, SDIO_CCCR_CAPS, &b)) {
			sdhci_reset(host, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
			continue;
		}
		if (b == priv->known_cccr)
			goto out;
		if (b != priv->known_cis) {
			dev_warn(mmc_dev(host->mmc), "buffer check: %08x %08x, expected %08x %08x\n",
				 a, b, priv->known_cis, priv->known_cccr);
			goto out;
		}
		/* one word late: a read left unread across a reset swaps them */
		s5l_raw_read4(host, SDIO_CCCR_CAPS, NULL);
		sdhci_reset(host, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
		priv->realigned++;
		dev_info_ratelimited(mmc_dev(host->mmc),
				     "data buffer one word late after an error: realigned (%u)\n",
				     priv->realigned);
	}
	dev_err(mmc_dev(host->mmc), "data buffer could not be realigned\n");
out:
	sdhci_writel(host, ~0, SDHCI_INT_STATUS);
	sdhci_writel(host, sig, SDHCI_SIGNAL_ENABLE);
}

/* REQUEST_ERROR resets the CMD line and then the DAT line on its own */
static void sdhci_apple_s5l_reset(struct sdhci_host *host, u8 mask)
{
	sdhci_reset(host, mask);
	if (mask == SDHCI_RESET_DATA)
		sdhci_apple_s5l_resync(host);
}

static const struct sdhci_ops sdhci_apple_s5l_ops = {
	.set_clock		= sdhci_apple_s5l_set_clock,
	.set_bus_width		= sdhci_set_bus_width,
	.reset			= sdhci_apple_s5l_reset,
	.set_uhs_signaling	= sdhci_set_uhs_signaling,
	.get_max_clock		= sdhci_apple_s5l_get_max_clock,
	.irq			= sdhci_apple_s5l_irq,
};

static const struct sdhci_pltfm_data sdhci_apple_s5l_pdata = {
	.ops	= &sdhci_apple_s5l_ops,
	.quirks2 = SDHCI_QUIRK2_NO_1_8_V,
};

static int sdhci_apple_s5l_probe(struct platform_device *pdev)
{
	struct sdhci_pltfm_host *pltfm_host;
	struct sdhci_apple_s5l *priv;
	struct sdhci_host *host;
	u32 caps, ver;
	int ret;

	host = sdhci_pltfm_init(pdev, &sdhci_apple_s5l_pdata, sizeof(*priv));
	if (IS_ERR(host))
		return PTR_ERR(host);
	pltfm_host = sdhci_priv(host);
	priv = sdhci_pltfm_priv(pltfm_host);

	/* The PMGR power state first: until it is on, the window reads what
	 * the bus hands back for a dead address. */
	priv->gate = devm_clk_get_optional_enabled(&pdev->dev, "gate");
	if (IS_ERR(priv->gate)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(priv->gate), "no power state\n");
		goto err;
	}

	ver = sdhci_readw(host, SDHCI_HOST_VERSION);
	caps = sdhci_readl(host, SDHCI_CAPABILITIES);
	dev_info(&pdev->dev, "SDHC version %#06x, capabilities %#010x %#010x, present %#010x\n",
		 ver, caps, sdhci_readl(host, SDHCI_CAPABILITIES_1),
		 sdhci_readl(host, SDHCI_PRESENT_STATE));

	if (hold) {
		dev_info(&pdev->dev, "hold=1: powered, no MMC host (release=1 starts it)\n");
		held = pdev;
		return 0;
	}
	return sdhci_apple_s5l_start(pdev);

err:
	sdhci_pltfm_free(pdev);
	return ret;
}

static int sdhci_apple_s5l_start(struct platform_device *pdev)
{
	struct sdhci_host *host = platform_get_drvdata(pdev);
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_apple_s5l *priv = sdhci_pltfm_priv(pltfm_host);
	u32 caps = sdhci_readl(host, SDHCI_CAPABILITIES);
	u16 spec;
	int ret;

	priv->base_hz = base_hz;
	if (!priv->base_hz)
		of_property_read_u32(pdev->dev.of_node, "clock-frequency", &priv->base_hz);
	if (!priv->base_hz)
		priv->base_hz = FIELD_GET(SDHCI_CLOCK_BASE_MASK, caps) * 1000000;
	if (!priv->base_hz) {
		ret = dev_err_probe(&pdev->dev, -EINVAL,
				    "no base clock: the capabilities give none and the DT has no clock-frequency\n");
		goto err;
	}
	host->quirks |= SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN;
	if (!use_dma)
		host->quirks |= SDHCI_QUIRK_BROKEN_DMA | SDHCI_QUIRK_BROKEN_ADMA;

	/*
	 * A version 2.00 host, whatever the version register says (3.00).
	 * The IOP firmware drives it as one -- power-of-two divisors, no Host
	 * Control 2 -- and a write to Host Control 2 (0x3e), which the SDHCI
	 * core makes for a 3.00 host whenever it sets the bus timing, stops
	 * the card clock dead: clock control reads 0 straight after it, and
	 * every command then times out with no interrupt (first boots,
	 * 2026-11-09; a 16-bit write of 0 there from userspace did the same).
	 */
	spec = SDHCI_SPEC_200 << SDHCI_SPEC_VER_SHIFT;
	__sdhci_read_caps(host, &spec, NULL, NULL);

	ret = mmc_of_parse(host->mmc);
	if (ret)
		goto err;
	sdhci_get_of_property(pdev);
	host->mmc_host_ops.init_card = sdhci_apple_s5l_init_card;

	ret = sdhci_add_host(host);
	if (ret)
		goto err;
	dev_info(&pdev->dev, "base clock %u Hz, %s\n", priv->base_hz,
		 use_dma ? "the controller's DMA" : "PIO");
	return 0;

err:
	sdhci_pltfm_free(pdev);
	return ret;
}

static const struct of_device_id sdhci_apple_s5l_of_match[] = {
	{ .compatible = "apple,s5l8940x-sdio" },
	{ }
};
MODULE_DEVICE_TABLE(of, sdhci_apple_s5l_of_match);

static struct platform_driver sdhci_apple_s5l_driver = {
	.driver = {
		.name = "sdhci-apple-s5l",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = sdhci_apple_s5l_of_match,
		.pm = &sdhci_pltfm_pmops,
	},
	.probe = sdhci_apple_s5l_probe,
	.remove = sdhci_pltfm_remove,
};
module_platform_driver(sdhci_apple_s5l_driver);

MODULE_DESCRIPTION("SDHCI driver for the Apple S5L89xx SDIO host");
MODULE_LICENSE("GPL");
