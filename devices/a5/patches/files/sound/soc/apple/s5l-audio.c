// SPDX-License-Identifier: GPL-2.0-only
/*
 * The iPad mini 1's speakers: A5 (S5L8942X) MCA0 + CDMA + two CS35L19 amps.
 *
 * Everything here was worked out for this port and is written down in
 * docs/research/p105-audio.md; in short:
 *
 *   MCA0 (0x34196000)   the audio port the speakers' data come from: clock
 *                       master, 2 x 32-bit slots, registers as iOS's
 *                       AppleAE2MCA writes them (computed by running iOS's
 *                       own code, tools/audio/mca-emu.py)
 *   NCOs (PMGR +0x110)  the MCLK generators, 342 MHz reference; MCA0 runs
 *                       from NCO 6 at 128 fs
 *   AE2 MCS/ACS         per-port kernel-clock source and register-clock gate
 *   i2s-switch          routes MCA0's clocks and data to pin group 1, and
 *                       I2S1's MCK to the amps
 *   I2S1 (0x34191000)   used only for its MCK: the amps need 12.000 MHz
 *                       before they power up.  Offered as a clock ("mclk")
 *                       for the amp driver.
 *   CDMA (0x37000000)   feeds MCA0's TX FIFO from a ring of commands; the
 *                       channel gives no interrupt while it runs, so the
 *                       period clock is an hrtimer reading MAR.
 *
 * This driver is also the machine driver: its card links MCA0 to the amps
 * named in "apple,speaker-amps", one 32-bit TDM slot each.  The card lives
 * on a child device: the amps take their MCLK from this device, so it has to
 * finish probing before they can, and the card has to wait for them.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/hrtimer.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

/* MCA (AppleAE2MCA) */
#define MCA_FMT			0x08	/* 32 - sample bits, TX | RX << 8 */
#define MCA_FRAME		0x0c
#define MCA_CTL			0x10
#define MCA_TX_MODE		0x14
#define MCA_TX_CFG		0x18	/* bit 0: TX on */
#define MCA_TX_C1		0x1c
#define MCA_TX_FIFO_CFG		0x20
#define MCA_TX_SLOTS		0x24
#define MCA_TX_FIFO		0x28
#define MCA_RX_CFG		0x2c
#define MCA_RX_C1		0x30
#define MCA_RX_FIFO_CFG		0x34
#define MCA_RX_SLOTS		0x38
#define MCA_STATUS		0x40
#define MCA_RESET		0x44

/* I2S (S5L8900 heritage, version 3) */
#define I2S_CLKCON		0x00
#define I2S_TXCON		0x04
#define I2S_TXCOM		0x08
#define I2S_RXCON		0x30
#define I2S_RXCOM		0x34
#define I2S_STATUS		0x3c

/* AE2 */
#define AE2_LANE(i)		(0x10 + 4 * (i))	/* MCS: 1 = own NCO; ACS: 1 = gated */
#define AE2_I2S1		1
#define AE2_MCA0		5

/* i2s-switch */
#define SW_PIN1			0x04
#define SW_PIN1_MCK		0x44

/* CDMA channel */
#define CDMA_CLK_ON(ch)		(4 * ((ch) / 32))
#define CDMA_CH(ch)		(0x1000 * (ch))
#define CDMA_CSR		0x00
#define CDMA_DCR		0x04
#define CDMA_DAR		0x08
#define CDMA_DBR		0x0c
#define CDMA_MAR		0x10
#define CDMA_CAR		0x14
#define CDMA_ERR		0x18
#define CDMA_CSR_ABORT		BIT(1)
#define CDMA_CSR_RUNNING	BIT(16)
#define CDMA_CSR_CLEAR		(BIT(18) | BIT(19) | BIT(20))
#define CDMA_CSR_START		(0x02000000 | CDMA_CSR_CLEAR | BIT(3) | BIT(0))
#define CDMA_CMD_DATA		0x02000103
#define CDMA_DBR_MAX		0x7ffffff0
#define CDMA_MAX_CMDS		64

#define S5L_NCO_MCA0		6
#define S5L_NCO_I2S1		1
#define S5L_MCLK_HZ		12000000

struct s5l_audio {
	struct device *dev;
	void __iomem *mca, *i2s, *mcs, *acs, *sw, *nco, *cdma;
	struct clk *mca_clk;
	struct clk_hw mclk;
	u32 nco_ref;
	u32 fifo;			/* MCA0's TX FIFO, physical */
	unsigned int ch, req;

	struct snd_pcm_substream *substream;
	__le32 *cmds;
	dma_addr_t cmds_dma;
	dma_addr_t buf_dma;
	unsigned int buf_bytes, period_bytes, periods;
	unsigned int width;		/* bytes per sample */
	unsigned int rate;
	unsigned int last_period;
	struct hrtimer timer;
	ktime_t tick;
	bool running;
	spinlock_t lock;

	struct platform_device *card_pdev;
	struct snd_soc_card card;
	struct snd_soc_dai_link link;
	struct snd_soc_dai_link_component cpu, platform, *codecs;
};

static int s5l_nco_set(struct s5l_audio *a, int n, u32 hz)
{
	void __iomem *r = a->nco + 16 * n;
	u32 v;
	int ret;

	writel(hz ? 0x90000000 : 0, r);
	ret = readl_poll_timeout_atomic(r, v, !(v & BIT(30)), 1, 1000);
	if (ret || !hz)
		return ret;
	writel(2 * hz, r + 4);
	ret = readl_poll_timeout_atomic(r, v, !(v & BIT(9)), 1, 1000);
	writel(2 * hz - a->nco_ref, r + 8);
	ret |= readl_poll_timeout_atomic(r, v, !(v & BIT(9)), 1, 1000);
	writel(0x90000400, r);
	ret |= readl_poll_timeout_atomic(r, v, !(v & BIT(9)), 1, 1000);
	writel(0x90000c00, r);
	ret |= readl_poll_timeout_atomic(r, v, !(v & BIT(9)), 1, 1000);
	return ret ? -ETIMEDOUT : 0;
}

/* --- the amps' MCLK: I2S1 as clock master, its MCK on pin1 ------------- */

static struct s5l_audio *mclk_to_audio(struct clk_hw *hw)
{
	return container_of(hw, struct s5l_audio, mclk);
}

static int s5l_mclk_enable(struct clk_hw *hw)
{
	struct s5l_audio *a = mclk_to_audio(hw);
	int ret;

	ret = s5l_nco_set(a, S5L_NCO_I2S1, S5L_MCLK_HZ);
	if (ret)
		return ret;
	writel(0, a->acs + AE2_LANE(AE2_I2S1));
	writel(1, a->mcs + AE2_LANE(AE2_I2S1));
	/* what iOS's configure writes: TXCON bit 7 = MCLK/BCLK ratio 1 */
	writel(1, a->i2s + I2S_STATUS);
	writel(0x03000081, a->i2s + I2S_TXCON);
	writel(0x1000, a->i2s + I2S_RXCON);
	writel(0, a->i2s + I2S_TXCOM);
	writel(0, a->i2s + I2S_RXCOM);
	writel(1, a->i2s + I2S_CLKCON);
	writel(1, a->sw + SW_PIN1_MCK);
	return 0;
}

static void s5l_mclk_disable(struct clk_hw *hw)
{
	struct s5l_audio *a = mclk_to_audio(hw);

	writel(0, a->sw + SW_PIN1_MCK);
	writel(0x34, a->i2s + I2S_CLKCON);
	s5l_nco_set(a, S5L_NCO_I2S1, 0);
}

static unsigned long s5l_mclk_recalc_rate(struct clk_hw *hw, unsigned long parent)
{
	return S5L_MCLK_HZ;
}

static const struct clk_ops s5l_mclk_ops = {
	.enable = s5l_mclk_enable,
	.disable = s5l_mclk_disable,
	.recalc_rate = s5l_mclk_recalc_rate,
};

/* --- MCA0 ----------------------------------------------------------------- */

/*
 * AppleAE2MCA's configure for: clock master, 2 x 32-bit slots, the speakers'
 * format flags (0x12), 48 kHz class rates; 16-bit samples sit in the top of
 * their slots.  The reset at +0x44 returns every register to its default.
 */
static int s5l_mca_configure(struct s5l_audio *a)
{
	u32 v;
	int ret;

	writel(0, a->mca + MCA_TX_CFG);
	writel(0x301, a->mca + MCA_RESET);
	ret = readl_poll_timeout(a->mca + MCA_RESET, v, !v, 10, 10000);
	if (ret)
		return ret;
	writel(0, a->mca + MCA_CTL);
	writel(0xd, a->mca + MCA_TX_C1);
	writel(0xd, a->mca + MCA_RX_C1);
	writel(a->width == 2 ? 0x1010 : 0, a->mca + MCA_FMT);
	/*
	 * iOS asks for DMA at 4 words left (the low byte); 16 gives the
	 * CDMA ~170 us instead of ~40 to answer, and costs nothing
	 */
	writel(0x002f0010, a->mca + MCA_TX_FIFO_CFG);
	writel(0x002f0004, a->mca + MCA_RX_FIFO_CFG);
	writel(0, a->mca + MCA_TX_MODE);
	writel(3, a->mca + MCA_TX_SLOTS);
	writel(3, a->mca + MCA_RX_SLOTS);
	writel(0x208, a->mca + MCA_TX_CFG);
	writel(0x10a, a->mca + MCA_RX_CFG);
	writel(1, a->mca + MCA_CTL);
	writel(0xb0003000, a->mca + MCA_STATUS);
	writel(0x0f401007, a->mca + MCA_FRAME);
	return 0;
}

/* --- CDMA ----------------------------------------------------------------- */

static void __iomem *s5l_ch(struct s5l_audio *a)
{
	return a->cdma + CDMA_CH(a->ch);
}

static void s5l_dma_stop(struct s5l_audio *a)
{
	void __iomem *ch = s5l_ch(a);

	writel(CDMA_CSR_ABORT, ch + CDMA_CSR);
	udelay(10);
	writel(CDMA_CSR_CLEAR, ch + CDMA_CSR);
}

/* start (or restart) the ring at command @first */
static int s5l_dma_start(struct s5l_audio *a, unsigned int first)
{
	void __iomem *ch = s5l_ch(a);
	u32 dcr;

	/* width (2 or 4 bytes), burst 4, memory to device, the port's request */
	dcr = (a->req << 16) | 0x20 | (a->width == 2 ? 0x04 : 0x08) | 0x02;

	writel(BIT(a->ch % 32), a->cdma + CDMA_CLK_ON(a->ch));
	writel(CDMA_CSR_CLEAR, ch + CDMA_CSR);
	writel(CDMA_CSR_ABORT, ch + CDMA_CSR);
	udelay(10);
	writel(dcr, ch + CDMA_DCR);
	writel(a->fifo & 0xffffff, ch + CDMA_DAR);
	writel(CDMA_DBR_MAX, ch + CDMA_DBR);
	writel(a->cmds_dma + 32 * first, ch + CDMA_CAR);
	if ((readl(ch + CDMA_DCR) & 0x003f00fe) != dcr) {
		dev_err(a->dev, "CDMA channel %u is locked (DCR %08x)\n",
			a->ch, readl(ch + CDMA_DCR));
		return -EBUSY;
	}
	writel(CDMA_CSR_START, ch + CDMA_CSR);
	return 0;
}

static void s5l_build_ring(struct s5l_audio *a)
{
	unsigned int i;

	for (i = 0; i < a->periods; i++) {
		__le32 *c = a->cmds + 8 * i;

		c[0] = cpu_to_le32(a->cmds_dma + 32 * ((i + 1) % a->periods));
		c[1] = cpu_to_le32(CDMA_CMD_DATA);
		c[2] = cpu_to_le32(a->buf_dma + i * a->period_bytes);
		c[3] = cpu_to_le32(a->period_bytes);
	}
	wmb();
}

static unsigned int s5l_dma_pos(struct s5l_audio *a)
{
	u32 mar = readl(s5l_ch(a) + CDMA_MAR);

	if (mar < a->buf_dma || mar >= a->buf_dma + a->buf_bytes)
		return 0;
	return mar - a->buf_dma;
}

static enum hrtimer_restart s5l_tick(struct hrtimer *t)
{
	struct s5l_audio *a = container_of(t, struct s5l_audio, timer);
	struct snd_pcm_substream *ss;
	unsigned int period;
	unsigned long flags;
	u32 csr;

	spin_lock_irqsave(&a->lock, flags);
	if (!a->running) {
		spin_unlock_irqrestore(&a->lock, flags);
		return HRTIMER_NORESTART;
	}
	ss = a->substream;
	period = s5l_dma_pos(a) / a->period_bytes;
	csr = readl(s5l_ch(a) + CDMA_CSR);
	if (!(csr & CDMA_CSR_RUNNING)) {
		/* the byte count ran out (2 GB, ~90 minutes) or it hit an error */
		dev_dbg(a->dev, "CDMA stopped (CSR %08x ERR %08x), restarting\n",
			csr, readl(s5l_ch(a) + CDMA_ERR));
		s5l_dma_start(a, (period + 1) % a->periods);
	}
	spin_unlock_irqrestore(&a->lock, flags);

	if (period != a->last_period) {
		a->last_period = period;
		snd_pcm_period_elapsed(ss);
	}
	hrtimer_forward_now(t, a->tick);
	return HRTIMER_RESTART;
}

/* --- PCM ------------------------------------------------------------------ */

static const struct snd_pcm_hardware s5l_pcm_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S32_LE,
	.rates = SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |
		 SNDRV_PCM_RATE_48000,
	.rate_min = 32000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = 512 * 1024,
	.period_bytes_min = 256,
	.period_bytes_max = 64 * 1024,
	.periods_min = 2,
	.periods_max = CDMA_MAX_CMDS,
};

static int s5l_pcm_open(struct snd_soc_component *component,
			struct snd_pcm_substream *substream)
{
	struct s5l_audio *a = snd_soc_component_get_drvdata(component);
	struct snd_pcm_runtime *rt = substream->runtime;
	int ret;

	snd_soc_set_runtime_hwparams(substream, &s5l_pcm_hw);
	/* a burst is 4 transfers of up to 4 bytes */
	ret = snd_pcm_hw_constraint_step(rt, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 16);
	if (ret)
		return ret;
	ret = snd_pcm_hw_constraint_integer(rt, SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		return ret;
	ret = clk_prepare_enable(a->mca_clk);
	if (ret)
		return ret;
	a->substream = substream;
	return 0;
}

static int s5l_pcm_close(struct snd_soc_component *component,
			 struct snd_pcm_substream *substream)
{
	struct s5l_audio *a = snd_soc_component_get_drvdata(component);

	hrtimer_cancel(&a->timer);
	clk_disable_unprepare(a->mca_clk);
	a->substream = NULL;
	return 0;
}

static int s5l_pcm_hw_params(struct snd_soc_component *component,
			     struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params)
{
	struct s5l_audio *a = snd_soc_component_get_drvdata(component);

	a->width = params_width(params) / 8;
	a->rate = params_rate(params);
	a->period_bytes = params_period_bytes(params);
	a->periods = params_periods(params);
	return 0;
}

static int s5l_pcm_hw_free(struct snd_soc_component *component,
			   struct snd_pcm_substream *substream)
{
	struct s5l_audio *a = snd_soc_component_get_drvdata(component);

	hrtimer_cancel(&a->timer);
	return 0;
}

static int s5l_pcm_prepare(struct snd_soc_component *component,
			   struct snd_pcm_substream *substream)
{
	struct s5l_audio *a = snd_soc_component_get_drvdata(component);
	struct snd_pcm_runtime *rt = substream->runtime;
	int ret;

	hrtimer_cancel(&a->timer);
	s5l_dma_stop(a);
	a->buf_dma = rt->dma_addr;
	a->buf_bytes = snd_pcm_lib_buffer_bytes(substream);
	a->last_period = 0;

	/* MCA0 from its own NCO at 128 fs (MCLK = 2 x BCLK, 64 BCLKs a frame) */
	writel(0, a->acs + AE2_LANE(AE2_MCA0));
	writel(1, a->mcs + AE2_LANE(AE2_MCA0));
	ret = s5l_nco_set(a, S5L_NCO_MCA0, 128 * a->rate);
	if (ret)
		return ret;
	ret = s5l_mca_configure(a);
	if (ret)
		return ret;
	/* pin1: MCA0's frame clocks and data */
	writel(0xf014, a->sw + SW_PIN1);
	s5l_build_ring(a);

	/* twice a period, at least once every 10 ms */
	a->tick = ns_to_ktime(min_t(u64, div_u64((u64)NSEC_PER_SEC *
				   bytes_to_frames(rt, a->period_bytes), 2 * a->rate),
				   10 * NSEC_PER_MSEC));
	return 0;
}

static int s5l_pcm_trigger(struct snd_soc_component *component,
			   struct snd_pcm_substream *substream, int cmd)
{
	struct s5l_audio *a = snd_soc_component_get_drvdata(component);
	unsigned long flags;
	int ret = 0;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		spin_lock_irqsave(&a->lock, flags);
		ret = s5l_dma_start(a, a->last_period);
		if (!ret) {
			/* the DMA fills the FIFO, then the port starts */
			writel(readl(a->mca + MCA_TX_CFG) | 1, a->mca + MCA_TX_CFG);
			a->running = true;
		}
		spin_unlock_irqrestore(&a->lock, flags);
		if (!ret)
			hrtimer_start(&a->timer, a->tick, HRTIMER_MODE_REL);
		return ret;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		spin_lock_irqsave(&a->lock, flags);
		a->running = false;
		writel(readl(a->mca + MCA_TX_CFG) & ~1, a->mca + MCA_TX_CFG);
		s5l_dma_stop(a);
		spin_unlock_irqrestore(&a->lock, flags);
		/* may be called from the timer itself (an xrun): don't wait */
		hrtimer_try_to_cancel(&a->timer);
		return 0;
	}
	return -EINVAL;
}

static snd_pcm_uframes_t s5l_pcm_pointer(struct snd_soc_component *component,
					 struct snd_pcm_substream *substream)
{
	struct s5l_audio *a = snd_soc_component_get_drvdata(component);

	return bytes_to_frames(substream->runtime, s5l_dma_pos(a));
}

static int s5l_pcm_construct(struct snd_soc_component *component,
			     struct snd_soc_pcm_runtime *rtd)
{
	return snd_pcm_set_managed_buffer_all(rtd->pcm, SNDRV_DMA_TYPE_DEV,
					      component->dev, 128 * 1024,
					      s5l_pcm_hw.buffer_bytes_max);
}

static const struct snd_soc_component_driver s5l_component = {
	.name = "s5l-audio",
	.open = s5l_pcm_open,
	.close = s5l_pcm_close,
	.hw_params = s5l_pcm_hw_params,
	.hw_free = s5l_pcm_hw_free,
	.prepare = s5l_pcm_prepare,
	.trigger = s5l_pcm_trigger,
	.pointer = s5l_pcm_pointer,
	.pcm_construct = s5l_pcm_construct,
};

static struct snd_soc_dai_driver s5l_dai = {
	.name = "s5l-mca0",
	.playback = {
		.stream_name = "MCA0 Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |
			 SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S32_LE,
	},
};

/* --- the card --------------------------------------------------------------- */

static int s5l_card_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dai *codec_dai;
	int i, ret;

	/* amp n takes 32-bit slot n */
	for_each_rtd_codec_dais(rtd, i, codec_dai) {
		ret = snd_soc_dai_set_tdm_slot(codec_dai, 0, BIT(i), 2, 32);
		if (ret)
			return ret;
	}
	return 0;
}

static int s5l_card_setup(struct s5l_audio *a, struct device *dev)
{
	struct device_node *np = a->dev->of_node;
	struct snd_soc_dai_link *l = &a->link;
	int n, i;

	n = of_count_phandle_with_args(np, "apple,speaker-amps", NULL);
	if (n <= 0)
		return dev_err_probe(a->dev, -EINVAL, "no apple,speaker-amps\n");
	a->codecs = devm_kcalloc(a->dev, n, sizeof(*a->codecs), GFP_KERNEL);
	if (!a->codecs)
		return -ENOMEM;
	for (i = 0; i < n; i++) {
		a->codecs[i].of_node = of_parse_phandle(np, "apple,speaker-amps", i);
		a->codecs[i].dai_name = "cs35l19";
	}

	a->cpu.of_node = np;
	a->cpu.dai_name = s5l_dai.name;
	a->platform.of_node = np;
	l->name = "Speakers";
	l->stream_name = "Speakers";
	l->cpus = &a->cpu;
	l->num_cpus = 1;
	l->codecs = a->codecs;
	l->num_codecs = n;
	l->platforms = &a->platform;
	l->num_platforms = 1;
	l->init = s5l_card_init;
	l->playback_only = 1;

	a->card.dev = dev;
	a->card.owner = THIS_MODULE;
	a->card.name = "iPadMini";
	a->card.long_name = "iPad mini 1 speakers";
	a->card.driver_name = "s5l-audio";
	a->card.dai_link = l;
	a->card.num_links = 1;
	return devm_snd_soc_register_card(dev, &a->card);
}

static int s5l_card_probe(struct platform_device *pdev)
{
	struct s5l_audio *a = *(struct s5l_audio **)dev_get_platdata(&pdev->dev);

	return s5l_card_setup(a, &pdev->dev);
}

static struct platform_driver s5l_card_driver = {
	.driver = {
		.name = "s5l-audio-card",
	},
	.probe = s5l_card_probe,
};

static void s5l_card_unregister(void *data)
{
	platform_device_unregister(data);
}

static void s5l_put_codecs(void *data)
{
	struct s5l_audio *a = data;
	unsigned int i;

	for (i = 0; a->codecs && i < a->link.num_codecs; i++)
		of_node_put(a->codecs[i].of_node);
}

static int s5l_audio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_init_data init = {};
	struct clk_parent_data parent = { .index = 1 };	/* "i2s" */
	struct resource *res;
	struct s5l_audio *a;
	int ret;

	a = devm_kzalloc(dev, sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;
	a->dev = dev;
	spin_lock_init(&a->lock);
	platform_set_drvdata(pdev, a);

	/* devm_ioremap, not _resource: the windows sit inside other blocks */
#define MAP(field, name) do {							\
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);	\
		if (!res)							\
			return dev_err_probe(dev, -EINVAL, "no %s window\n", name); \
		a->field = devm_ioremap(dev, res->start, resource_size(res));	\
		if (!a->field)							\
			return -ENOMEM;						\
	} while (0)
	MAP(mca, "mca");
	MAP(i2s, "i2s");
	MAP(mcs, "mcs");
	MAP(acs, "acs");
	MAP(sw, "switch");
	MAP(nco, "nco");
	MAP(cdma, "cdma");
#undef MAP
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mca");
	a->fifo = res->start + MCA_TX_FIFO;

	if (of_property_read_u32(dev->of_node, "apple,nco-reference-hz", &a->nco_ref))
		a->nco_ref = 342000000;
	if (of_property_read_u32_index(dev->of_node, "apple,dma-channel", 0, &a->ch) ||
	    of_property_read_u32_index(dev->of_node, "apple,dma-channel", 1, &a->req))
		return dev_err_probe(dev, -EINVAL, "no apple,dma-channel\n");

	a->mca_clk = devm_clk_get(dev, "mca");
	if (IS_ERR(a->mca_clk))
		return dev_err_probe(dev, PTR_ERR(a->mca_clk), "no MCA power state\n");

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	a->cmds = dmam_alloc_coherent(dev, 32 * CDMA_MAX_CMDS, &a->cmds_dma, GFP_KERNEL);
	if (!a->cmds)
		return -ENOMEM;

	hrtimer_init(&a->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	a->timer.function = s5l_tick;

	init.name = "s5l-spk-mclk";
	init.ops = &s5l_mclk_ops;
	init.parent_data = &parent;
	init.num_parents = 1;
	a->mclk.init = &init;
	ret = devm_clk_hw_register(dev, &a->mclk);
	if (ret)
		return ret;
	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get, &a->mclk);
	if (ret)
		return ret;

	ret = devm_snd_soc_register_component(dev, &s5l_component, &s5l_dai, 1);
	if (ret)
		return ret;
	ret = devm_add_action(dev, s5l_put_codecs, a);
	if (ret)
		return ret;

	a->card_pdev = platform_device_register_data(dev, "s5l-audio-card",
						     PLATFORM_DEVID_AUTO, &a, sizeof(a));
	if (IS_ERR(a->card_pdev))
		return PTR_ERR(a->card_pdev);
	return devm_add_action_or_reset(dev, s5l_card_unregister, a->card_pdev);
}

static const struct of_device_id s5l_audio_of_match[] = {
	{ .compatible = "apple,s5l8942x-audio" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5l_audio_of_match);

static struct platform_driver s5l_audio_driver = {
	.driver = {
		.name = "s5l-audio",
		.of_match_table = s5l_audio_of_match,
	},
	.probe = s5l_audio_probe,
};
static struct platform_driver * const s5l_drivers[] = {
	&s5l_audio_driver,
	&s5l_card_driver,
};

static int __init s5l_audio_init(void)
{
	return platform_register_drivers(s5l_drivers, ARRAY_SIZE(s5l_drivers));
}
module_init(s5l_audio_init);

static void __exit s5l_audio_exit(void)
{
	platform_unregister_drivers(s5l_drivers, ARRAY_SIZE(s5l_drivers));
}
module_exit(s5l_audio_exit);

MODULE_DESCRIPTION("Apple A5 (S5L8942X) speaker audio: MCA0, CDMA, CS35L19 card");
MODULE_LICENSE("GPL");
