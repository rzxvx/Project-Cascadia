// SPDX-License-Identifier: GPL-2.0-only
/*
 * The iPad 2's audio: A5 (S5L8940X) I2S0 + CDMA, playing through the CS42L63
 * codec (sound/soc/codecs/cs42l63.c).
 *
 * docs/research/k93-audio.md has every value and where it came from (iOS's
 * AppleSamsungI2SController, AppleAE2I2SSwitch1 and the PMGR's NCO code, run
 * in an emulator); the iPad mini's s5l-audio.c is the model, and its CDMA ring
 * and hrtimer period clock are the same here.  There is no MCA on this SoC:
 *
 *   I2S0 (0x34190000)   clock master, 16-bit stereo I2S.  MCLK 6 MHz =
 *                       nclk (24 MHz) / 4, CLKCON bits 8:7; BCLK from NCO 0
 *   NCO 0 (PMGR +0x110) BCLK = 32 fs, 342 MHz reference
 *   AE2 ACS lane 0      I2S0's register-clock gate (0 = open)
 *   i2s-switch          +0x00 = 0x44: I2S0's frame clocks and TX data on pin
 *                       group 0; +0x30 = 1: its MCK on pin 0 -- the codec's
 *   AUDIO-CLK (PMGR)    0x80000001, as iOS leaves it
 *   CDMA ch 0x1f        request line 1, into I2S0's TX FIFO (+0x10), 16-bit
 *
 * The codec needs MCLK before it leaves reset, so MCLK is this device's
 * clock (#clock-cells = <0>), the I2S0 power state its parent; the card
 * lives on a child device, registered once this one has probed, and waits
 * for the codec.
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

/* I2S (S5L8900 heritage, version 3) */
#define I2S_CLKCON		0x00
#define I2S_TXCON		0x04
#define I2S_TXCOM		0x08
#define I2S_TXFIFO		0x10
#define I2S_RXCON		0x30
#define I2S_RXCOM		0x34
#define I2S_STATUS		0x3c

#define I2S_CLKCON_IDLE		0x1b4	/* stopped; MCK = nclk / 4 runs */
#define I2S_CLKCON_RUN		0x181	/* running; BCLK = the NCO */
#define I2S_CLKCON_OFF		0x34
#define I2S_TXCON_16		0x03000081
#define I2S_RXCON_16		0x1000
#define I2S_TXCOM_GO		0x01feca06

/* i2s-switch (AppleAE2I2SSwitch1), pin group 0 */
#define SW_PIN0			0x00
#define SW_PIN0_TX		0x44	/* frame clocks driven by ap0, its TX data */
#define SW_PIN0_MCK		0x30

/* AE2 ACS: lane 0 is I2S0 */
#define AE2_ACS_LANE0		0x10

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

#define S5L_NCO_I2S0		0
#define S5L_MCLK_HZ		6000000
#define S5L_AUDIOCLK_ON		0x80000001

struct s5l8940_audio {
	struct device *dev;
	void __iomem *i2s, *acs, *sw, *nco, *cdma, *audioclk;
	struct clk *i2s_clk;
	struct clk_hw mclk;
	u32 nco_ref;
	u32 fifo;			/* I2S0's TX FIFO, physical */
	unsigned int ch, req;

	struct snd_pcm_substream *substream;
	__le32 *cmds;
	dma_addr_t cmds_dma;
	dma_addr_t buf_dma;
	unsigned int buf_bytes, period_bytes, periods;
	unsigned int rate;
	unsigned int last_period;
	struct hrtimer timer;
	ktime_t tick;
	bool running;
	spinlock_t lock;

	struct platform_device *card_pdev;
	struct snd_soc_card card;
	struct snd_soc_dai_link link;
	struct snd_soc_dai_link_component cpu, platform, codec;
};

static int s5l_nco_set(struct s5l8940_audio *a, int n, u32 hz)
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

/* --- MCLK for the codec: I2S0 idle, its MCK on pin 0 ------------------------ */

static struct s5l8940_audio *mclk_to_audio(struct clk_hw *hw)
{
	return container_of(hw, struct s5l8940_audio, mclk);
}

/* what iOS's setMCLKEnable(1) leaves: TXCOM = RXCOM = 0, CLKCON = 0x1b4 */
static int s5l_mclk_enable(struct clk_hw *hw)
{
	struct s5l8940_audio *a = mclk_to_audio(hw);
	u32 v;

	writel(0, a->acs + AE2_ACS_LANE0);
	writel(0, a->i2s + I2S_TXCOM);
	writel(0, a->i2s + I2S_RXCOM);
	writel(I2S_CLKCON_IDLE, a->i2s + I2S_CLKCON);
	/* bit 1 reads 1 once the port has stopped */
	if (readl_poll_timeout_atomic(a->i2s + I2S_CLKCON, v, v & BIT(1), 1, 1000))
		dev_warn(a->dev, "I2S0 did not stop (CLKCON %08x)\n", v);
	writel(1, a->sw + SW_PIN0_MCK);
	return 0;
}

static void s5l_mclk_disable(struct clk_hw *hw)
{
	struct s5l8940_audio *a = mclk_to_audio(hw);

	writel(0, a->sw + SW_PIN0_MCK);
	writel(I2S_CLKCON_OFF, a->i2s + I2S_CLKCON);
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

/* --- I2S0 -------------------------------------------------------------------- */

/* iOS's configure for 16-bit stereo, then BCLK; started by trigger */
static int s5l_i2s_configure(struct s5l8940_audio *a)
{
	writel(0, a->acs + AE2_ACS_LANE0);
	writel(1, a->i2s + I2S_STATUS);
	writel(I2S_TXCON_16, a->i2s + I2S_TXCON);
	writel(I2S_RXCON_16, a->i2s + I2S_RXCON);
	writel(0, a->i2s + I2S_TXCOM);
	writel(0, a->i2s + I2S_RXCOM);
	writel(I2S_CLKCON_RUN, a->i2s + I2S_CLKCON);
	writel(SW_PIN0_TX, a->sw + SW_PIN0);
	writel(1, a->sw + SW_PIN0_MCK);
	return s5l_nco_set(a, S5L_NCO_I2S0, 32 * a->rate);
}

/* back to MCLK only, which the codec keeps */
static void s5l_i2s_idle(struct s5l8940_audio *a)
{
	writel(0, a->i2s + I2S_TXCOM);
	writel(0, a->i2s + I2S_RXCOM);
	writel(I2S_CLKCON_IDLE, a->i2s + I2S_CLKCON);
	writel(0, a->sw + SW_PIN0);
	s5l_nco_set(a, S5L_NCO_I2S0, 0);
}

/* --- CDMA -------------------------------------------------------------------- */

static void __iomem *s5l_ch(struct s5l8940_audio *a)
{
	return a->cdma + CDMA_CH(a->ch);
}

static void s5l_dma_stop(struct s5l8940_audio *a)
{
	void __iomem *ch = s5l_ch(a);

	writel(CDMA_CSR_ABORT, ch + CDMA_CSR);
	udelay(10);
	writel(CDMA_CSR_CLEAR, ch + CDMA_CSR);
}

/* start (or restart) the ring at command @first */
static int s5l_dma_start(struct s5l8940_audio *a, unsigned int first)
{
	void __iomem *ch = s5l_ch(a);
	u32 dcr;

	/* 16-bit, burst 4, memory to device, the port's request line */
	dcr = (a->req << 16) | 0x20 | 0x04 | 0x02;

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

static void s5l_build_ring(struct s5l8940_audio *a)
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

static unsigned int s5l_dma_pos(struct s5l8940_audio *a)
{
	u32 mar = readl(s5l_ch(a) + CDMA_MAR);

	if (mar < a->buf_dma || mar >= a->buf_dma + a->buf_bytes)
		return 0;
	return mar - a->buf_dma;
}

static enum hrtimer_restart s5l_tick(struct hrtimer *t)
{
	struct s5l8940_audio *a = container_of(t, struct s5l8940_audio, timer);
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
		/* the byte count ran out (2 GB) or it hit an error */
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

/* --- PCM --------------------------------------------------------------------- */

static const struct snd_pcm_hardware s5l_pcm_hw = {
	.info = SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_MMAP |
		SNDRV_PCM_INFO_MMAP_VALID | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000,
	.rate_min = 44100,
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
	struct s5l8940_audio *a = snd_soc_component_get_drvdata(component);
	struct snd_pcm_runtime *rt = substream->runtime;
	int ret;

	snd_soc_set_runtime_hwparams(substream, &s5l_pcm_hw);
	/* a burst is 4 transfers of 2 bytes; whole frames */
	ret = snd_pcm_hw_constraint_step(rt, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, 16);
	if (ret)
		return ret;
	ret = snd_pcm_hw_constraint_integer(rt, SNDRV_PCM_HW_PARAM_PERIODS);
	if (ret < 0)
		return ret;
	ret = clk_prepare_enable(a->i2s_clk);
	if (ret)
		return ret;
	a->substream = substream;
	return 0;
}

static int s5l_pcm_close(struct snd_soc_component *component,
			 struct snd_pcm_substream *substream)
{
	struct s5l8940_audio *a = snd_soc_component_get_drvdata(component);

	hrtimer_cancel(&a->timer);
	s5l_i2s_idle(a);
	clk_disable_unprepare(a->i2s_clk);
	a->substream = NULL;
	return 0;
}

static int s5l_pcm_hw_params(struct snd_soc_component *component,
			     struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params)
{
	struct s5l8940_audio *a = snd_soc_component_get_drvdata(component);

	a->rate = params_rate(params);
	a->period_bytes = params_period_bytes(params);
	a->periods = params_periods(params);
	return 0;
}

static int s5l_pcm_hw_free(struct snd_soc_component *component,
			   struct snd_pcm_substream *substream)
{
	struct s5l8940_audio *a = snd_soc_component_get_drvdata(component);

	hrtimer_cancel(&a->timer);
	return 0;
}

static int s5l_pcm_prepare(struct snd_soc_component *component,
			   struct snd_pcm_substream *substream)
{
	struct s5l8940_audio *a = snd_soc_component_get_drvdata(component);
	struct snd_pcm_runtime *rt = substream->runtime;
	int ret;

	hrtimer_cancel(&a->timer);
	s5l_dma_stop(a);
	a->buf_dma = rt->dma_addr;
	a->buf_bytes = snd_pcm_lib_buffer_bytes(substream);
	a->last_period = 0;

	ret = s5l_i2s_configure(a);
	if (ret)
		return ret;
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
	struct s5l8940_audio *a = snd_soc_component_get_drvdata(component);
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
			writel(I2S_TXCOM_GO, a->i2s + I2S_TXCOM);
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
		writel(0, a->i2s + I2S_TXCOM);
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
	struct s5l8940_audio *a = snd_soc_component_get_drvdata(component);

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
	.name = "s5l8940-audio",
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
	.name = "s5l8940-i2s0",
	.playback = {
		.stream_name = "I2S0 Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
};

/* --- the card ------------------------------------------------------------------ */

static int s5l_card_setup(struct s5l8940_audio *a, struct device *dev)
{
	struct device_node *np = a->dev->of_node;
	struct snd_soc_dai_link *l = &a->link;

	a->codec.of_node = of_parse_phandle(np, "apple,codec", 0);
	if (!a->codec.of_node)
		return dev_err_probe(a->dev, -EINVAL, "no apple,codec\n");
	a->codec.dai_name = "cs42l63";
	a->cpu.of_node = np;
	a->cpu.dai_name = s5l_dai.name;
	a->platform.of_node = np;
	l->name = "Speaker";
	l->stream_name = "Speaker";
	l->cpus = &a->cpu;
	l->num_cpus = 1;
	l->codecs = &a->codec;
	l->num_codecs = 1;
	l->platforms = &a->platform;
	l->num_platforms = 1;
	l->dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF | SND_SOC_DAIFMT_CBC_CFC;
	l->playback_only = 1;

	a->card.dev = dev;
	a->card.owner = THIS_MODULE;
	a->card.name = "iPad2";
	a->card.long_name = "iPad 2 speaker";
	a->card.driver_name = "s5l8940-audio";
	a->card.dai_link = l;
	a->card.num_links = 1;
	return devm_snd_soc_register_card(dev, &a->card);
}

static int s5l_card_probe(struct platform_device *pdev)
{
	struct s5l8940_audio *a = *(struct s5l8940_audio **)dev_get_platdata(&pdev->dev);

	return s5l_card_setup(a, &pdev->dev);
}

static struct platform_driver s5l_card_driver = {
	.driver = {
		.name = "s5l8940-audio-card",
	},
	.probe = s5l_card_probe,
};

static void s5l_card_unregister(void *data)
{
	platform_device_unregister(data);
}

static void s5l_put_codec(void *data)
{
	struct s5l8940_audio *a = data;

	of_node_put(a->codec.of_node);
}

static int s5l8940_audio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_init_data init = {};
	struct clk_parent_data parent = { .index = 0 };	/* the I2S0 power state */
	struct resource *res;
	struct s5l8940_audio *a;
	u32 v;
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
	MAP(i2s, "i2s");
	MAP(acs, "acs");
	MAP(sw, "switch");
	MAP(nco, "nco");
	MAP(cdma, "cdma");
	MAP(audioclk, "audioclk");
#undef MAP
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "i2s");
	a->fifo = res->start + I2S_TXFIFO;

	if (of_property_read_u32(dev->of_node, "apple,nco-reference-hz", &a->nco_ref))
		a->nco_ref = 342000000;
	if (of_property_read_u32_index(dev->of_node, "apple,dma-channel", 0, &a->ch) ||
	    of_property_read_u32_index(dev->of_node, "apple,dma-channel", 1, &a->req))
		return dev_err_probe(dev, -EINVAL, "no apple,dma-channel\n");

	a->i2s_clk = devm_clk_get(dev, "i2s");
	if (IS_ERR(a->i2s_clk))
		return dev_err_probe(dev, PTR_ERR(a->i2s_clk), "no I2S0 power state\n");

	/* the audio block's clock, as iOS leaves it */
	writel(S5L_AUDIOCLK_ON, a->audioclk);
	if (readl_poll_timeout(a->audioclk, v, !(v & BIT(30)), 10, 10000))
		dev_warn(dev, "AUDIO-CLK stays busy (%08x)\n", v);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	a->cmds = dmam_alloc_coherent(dev, 32 * CDMA_MAX_CMDS, &a->cmds_dma, GFP_KERNEL);
	if (!a->cmds)
		return -ENOMEM;

	hrtimer_init(&a->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	a->timer.function = s5l_tick;

	init.name = "s5l8940-i2s0-mclk";
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
	ret = devm_add_action(dev, s5l_put_codec, a);
	if (ret)
		return ret;

	a->card_pdev = platform_device_register_data(dev, "s5l8940-audio-card",
						     PLATFORM_DEVID_AUTO, &a, sizeof(a));
	if (IS_ERR(a->card_pdev))
		return PTR_ERR(a->card_pdev);
	return devm_add_action_or_reset(dev, s5l_card_unregister, a->card_pdev);
}

static const struct of_device_id s5l8940_audio_of_match[] = {
	{ .compatible = "apple,s5l8940x-audio" },
	{ }
};
MODULE_DEVICE_TABLE(of, s5l8940_audio_of_match);

static struct platform_driver s5l8940_audio_driver = {
	.driver = {
		.name = "s5l8940-audio",
		.of_match_table = s5l8940_audio_of_match,
	},
	.probe = s5l8940_audio_probe,
};

static struct platform_driver * const s5l8940_drivers[] = {
	&s5l8940_audio_driver,
	&s5l_card_driver,
};

static int __init s5l8940_audio_init(void)
{
	return platform_register_drivers(s5l8940_drivers, ARRAY_SIZE(s5l8940_drivers));
}
module_init(s5l8940_audio_init);

static void __exit s5l8940_audio_exit(void)
{
	platform_unregister_drivers(s5l8940_drivers, ARRAY_SIZE(s5l8940_drivers));
}
module_exit(s5l8940_audio_exit);

MODULE_DESCRIPTION("Apple A5 (S5L8940X) I2S0 audio: CDMA, the iPad 2's CS42L63 card");
MODULE_LICENSE("GPL");
