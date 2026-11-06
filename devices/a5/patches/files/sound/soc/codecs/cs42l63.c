// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cirrus Logic CS42L63 codec, as the iPad 3 (J1) uses it: its serial port
 * (ASP) takes 16-bit I2S from the SoC's I2S0, and line out 2 drives the
 * speaker through an external amplifier with its own enable GPIO.
 *
 * The CS42L63 is Apple's part; no datasheet is public.  It is a sibling of
 * the CS42L73 (same device ID layout, rate codes and volume coding) with a
 * flat 8-bit register map of its own.  The sequences below are the register
 * writes iOS 9.3.5's AppleCS42L63Audio makes, recovered by running its code
 * in an emulator, and played back by hand on the device before this driver
 * was written (docs/research/j1-a5x.md).  What most registers mean is not
 * known, so they are named by number.
 *
 *   - the codec needs MCLK (6 MHz) before it powers up; it comes from the
 *     SoC's I2S0, the "mclk" clock;
 *   - only the speaker path is wired up here: no headphones, no
 *     microphones, no headset detection;
 *   - the amplifier enable goes on after the path is up and off before it
 *     goes down, as iOS orders them.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#define CS42L63_TEST_KEY	0x00
#define CS42L63_DEVID_AB	0x01	/* 0x42 0xa6 0x3x */
#define CS42L63_REV_ID		0x05
#define CS42L63_PWR1		0x06	/* bit 0: power down */
#define CS42L63_PWR2		0x07	/* output stage power downs */
#define CS42L63_SPC		0x11	/* bits 3:0: the sample rate code */
#define CS42L63_LO2_VOL_A	0x21
#define CS42L63_LO2_VOL_B	0x22
#define CS42L63_IS1		0x62	/* interrupt status, read to clear */
#define CS42L63_CTL_KEY		0x7d
#define CS42L63_MAX_REG		0x7f

#define CS42L63_VOL_MUTE	0x40
#define CS42L63_VOL_MIN_RAW	0x41	/* -76 dB */
#define CS42L63_VOL_MAX		62	/* 0x7f = -1 dB, iOS's maximum */
#define CS42L63_VOL_DEFAULT	41	/* -22 dB */

struct cs42l63 {
	struct device *dev;
	struct regmap *regmap;
	struct clk *mclk;
	struct gpio_desc *amp;
	unsigned int rate_code;
	unsigned int vol[2];		/* control values, 0 .. CS42L63_VOL_MAX */
	bool powered, speaker_on;
};

struct cs42l63_reg_upd {
	u8 reg, mask, val;
};

/* mask 0xff: a plain write */
#define W(r, v)		{ r, 0xff, v }
#define U(r, m, v)	{ r, m, v }

static int cs42l63_run(struct cs42l63 *cs, const struct cs42l63_reg_upd *seq,
		       unsigned int n)
{
	unsigned int i;
	int ret;

	for (i = 0; i < n; i++) {
		if (seq[i].mask == 0xff)
			ret = regmap_write(cs->regmap, seq[i].reg, seq[i].val);
		else
			ret = regmap_update_bits(cs->regmap, seq[i].reg, seq[i].mask,
						 seq[i].val);
		if (ret)
			return ret;
	}
	return 0;
}

/* AppleCS42L63Audio's powerUp, after the control key is released */
static const struct cs42l63_reg_upd cs42l63_power_up_seq[] = {
	W(CS42L63_TEST_KEY, 0x99), W(0x7e, 0x83), W(0x7f, 0xf0),
	W(CS42L63_TEST_KEY, 0x00),
	U(0x0a, 0x8f, 0x00),
	W(0x0c, 0x11), W(0x0e, 0x11), W(0x10, 0x91),	/* serial ports */
	U(0x0f, 0x10, 0x00),
	W(0x1c, 0x82), W(0x27, 0x0c),
	U(0x09, 0x07, 0x00),
	U(0x75, 0xc0, 0x80), W(0x73, 0xc0), U(0x71, 0x21, 0x20), W(0x76, 0x8f),
	U(CS42L63_PWR1, 0x01, 0x00),			/* out of power-down */
	W(0x12, 0xc9),
};

/* every output off and muted: what iOS leaves between streams */
static const struct cs42l63_reg_upd cs42l63_outputs_idle[] = {
	W(0x1d, 0x19),
	W(0x3d, 0x3f), W(0x3e, 0x3f), W(0x41, 0x7f), W(0x42, 0x3f),
	W(0x45, 0x3f), W(0x46, 0x3f), W(0x49, 0x7f), W(0x4a, 0x3f),
	U(0x3b, 0x07, 0x03), U(0x66, 0x03, 0x00), U(0x08, 0xac, 0xac),
	W(0x23, CS42L63_VOL_MUTE), W(0x25, CS42L63_VOL_MUTE),
	W(CS42L63_LO2_VOL_A, CS42L63_VOL_MUTE), W(CS42L63_LO2_VOL_B, CS42L63_VOL_MUTE),
	W(0x1f, 0x19), W(0x20, 0x19),
	U(CS42L63_PWR2, 0x7c, 0x7c),
};

/* the ASP into mixer group 0x44 at 0 dB, line out 2 up, the rest down */
static const struct cs42l63_reg_upd cs42l63_speaker_seq[] = {
	W(0x1d, 0x19),
	W(0x3d, 0x3f), W(0x3e, 0x3f), W(0x41, 0x7f), W(0x42, 0x3f),
	W(0x45, 0x00), W(0x46, 0x3f), W(0x49, 0x40), W(0x4a, 0x3f),
	U(0x3b, 0x07, 0x01), U(0x66, 0x0f, 0x00), U(0x08, 0xac, 0x2c),
	W(0x23, CS42L63_VOL_MUTE), W(0x25, CS42L63_VOL_MUTE),
	W(0x1f, 0x00), W(0x20, 0x00),
};

/* the first half of powerDown: outputs muted and off */
static const struct cs42l63_reg_upd cs42l63_power_down_seq[] = {
	U(0x12, 0xc0, 0x00),
	W(0x23, CS42L63_VOL_MUTE), W(0x25, CS42L63_VOL_MUTE),
	W(CS42L63_LO2_VOL_A, CS42L63_VOL_MUTE), W(CS42L63_LO2_VOL_B, CS42L63_VOL_MUTE),
	W(0x1d, 0x19), W(0x1f, 0x19), W(0x20, 0x19),
	W(CS42L63_PWR2, 0x7f),
	U(0x75, 0x80, 0x00),
	U(CS42L63_PWR1, 0x01, 0x01),			/* into power-down */
};

static unsigned int cs42l63_vol_raw(unsigned int v)
{
	return (v + CS42L63_VOL_MIN_RAW) & 0x7f;
}

static int cs42l63_write_volume(struct cs42l63 *cs)
{
	int ret;

	ret = regmap_write(cs->regmap, CS42L63_LO2_VOL_A, cs42l63_vol_raw(cs->vol[0]));
	if (ret)
		return ret;
	return regmap_write(cs->regmap, CS42L63_LO2_VOL_B, cs42l63_vol_raw(cs->vol[1]));
}

static int cs42l63_power_up(struct cs42l63 *cs)
{
	unsigned int st;
	int ret;

	/* the control key: set, 10 ms, released (iOS's cold power-up) */
	ret = regmap_write(cs->regmap, CS42L63_CTL_KEY, 0xa5);
	if (ret)
		return ret;
	usleep_range(10000, 11000);
	ret = regmap_write(cs->regmap, CS42L63_CTL_KEY, 0x00);
	if (ret)
		return ret;
	ret = cs42l63_run(cs, cs42l63_power_up_seq, ARRAY_SIZE(cs42l63_power_up_seq));
	if (ret)
		return ret;
	ret = regmap_update_bits(cs->regmap, CS42L63_SPC, 0x0f, cs->rate_code);
	if (ret)
		return ret;
	ret = cs42l63_run(cs, cs42l63_outputs_idle, ARRAY_SIZE(cs42l63_outputs_idle));
	if (ret)
		return ret;
	/* bit 7 latches once the codec runs from MCLK */
	regmap_read(cs->regmap, CS42L63_IS1, &st);
	dev_dbg(cs->dev, "powered up, status %02x\n", st);
	cs->powered = true;
	return 0;
}

static void cs42l63_power_down(struct cs42l63 *cs)
{
	unsigned int st = 0;
	int i;

	cs->powered = false;
	cs42l63_run(cs, cs42l63_power_down_seq, ARRAY_SIZE(cs42l63_power_down_seq));
	/* iOS waits for bit 7, 10 ms a try, and goes on without it */
	for (i = 0; i < 10; i++) {
		if (regmap_read(cs->regmap, CS42L63_IS1, &st) || (st & 0x80))
			break;
		usleep_range(10000, 11000);
	}
	regmap_update_bits(cs->regmap, CS42L63_PWR1, 0x40, 0x40);
	usleep_range(10000, 11000);
	regmap_write(cs->regmap, CS42L63_CTL_KEY, 0xa5);
}

static int cs42l63_power_event(struct snd_soc_dapm_widget *w,
			       struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct cs42l63 *cs = snd_soc_component_get_drvdata(component);
	int ret;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		/*
		 * Looked up here, not at probe: MCLK's provider is the sound
		 * card's own driver, which waits for this one to probe.
		 */
		if (!cs->mclk) {
			struct clk *mclk = devm_clk_get(cs->dev, "mclk");

			if (IS_ERR(mclk))
				return dev_err_probe(cs->dev, PTR_ERR(mclk), "no MCLK\n");
			cs->mclk = mclk;
		}
		ret = clk_prepare_enable(cs->mclk);
		if (ret)
			return ret;
		ret = cs42l63_power_up(cs);
		if (ret) {
			dev_err(cs->dev, "power-up failed: %d\n", ret);
			clk_disable_unprepare(cs->mclk);
		}
		return ret;
	case SND_SOC_DAPM_POST_PMD:
		cs42l63_power_down(cs);
		clk_disable_unprepare(cs->mclk);
		return 0;
	}
	return 0;
}

static int cs42l63_speaker_event(struct snd_soc_dapm_widget *w,
				 struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct cs42l63 *cs = snd_soc_component_get_drvdata(component);
	int ret;

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		ret = cs42l63_run(cs, cs42l63_speaker_seq, ARRAY_SIZE(cs42l63_speaker_seq));
		if (!ret)
			ret = cs42l63_write_volume(cs);
		if (!ret)
			ret = regmap_update_bits(cs->regmap, CS42L63_PWR2, 0x7c, 0x64);
		if (ret)
			return ret;
		cs->speaker_on = true;
		/* the amplifier last, as iOS does */
		gpiod_set_value_cansleep(cs->amp, 1);
		msleep(50);
		return 0;
	case SND_SOC_DAPM_PRE_PMD:
		/* the amplifier first */
		gpiod_set_value_cansleep(cs->amp, 0);
		cs->speaker_on = false;
		return cs42l63_run(cs, cs42l63_outputs_idle, ARRAY_SIZE(cs42l63_outputs_idle));
	}
	return 0;
}

/* -76 dB in 2 dB steps to -50, then 1 dB steps (the CS42L73's coding) */
static const DECLARE_TLV_DB_RANGE(cs42l63_vol_tlv,
	0, 13, TLV_DB_SCALE_ITEM(-7600, 200, 0),
	14, 75, TLV_DB_SCALE_ITEM(-4900, 100, 0)
);

/*
 * The volume is kept here, not read back: every power-up and every path
 * change writes the line-out registers to mute, as iOS's sequences do.
 */
static int cs42l63_vol_info(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 2;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = CS42L63_VOL_MAX;
	return 0;
}

static int cs42l63_vol_get(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct cs42l63 *cs = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = cs->vol[0];
	ucontrol->value.integer.value[1] = cs->vol[1];
	return 0;
}

static int cs42l63_vol_put(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct cs42l63 *cs = snd_soc_component_get_drvdata(component);
	long l = ucontrol->value.integer.value[0];
	long r = ucontrol->value.integer.value[1];
	int ret;

	if (l < 0 || l > CS42L63_VOL_MAX || r < 0 || r > CS42L63_VOL_MAX)
		return -EINVAL;
	if (l == cs->vol[0] && r == cs->vol[1])
		return 0;
	cs->vol[0] = l;
	cs->vol[1] = r;
	if (cs->speaker_on) {
		ret = cs42l63_write_volume(cs);
		if (ret)
			return ret;
	}
	return 1;
}

static const struct snd_kcontrol_new cs42l63_controls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Speaker Playback Volume",
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
			  SNDRV_CTL_ELEM_ACCESS_TLV_READ,
		.tlv.p = cs42l63_vol_tlv,
		.info = cs42l63_vol_info,
		.get = cs42l63_vol_get,
		.put = cs42l63_vol_put,
	},
};

static const struct snd_soc_dapm_widget cs42l63_widgets[] = {
	SND_SOC_DAPM_SUPPLY("Codec Power", SND_SOC_NOPM, 0, 0, cs42l63_power_event,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_AIF_IN("ASP", "ASP Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_OUT_DRV_E("LO2", SND_SOC_NOPM, 0, 0, NULL, 0,
			       cs42l63_speaker_event,
			       SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_OUTPUT("SPK"),
};

static const struct snd_soc_dapm_route cs42l63_routes[] = {
	{ "ASP", NULL, "Codec Power" },
	{ "LO2", NULL, "ASP" },
	{ "SPK", NULL, "LO2" },
};

/* the rate codes AppleCS42L63Audio writes to 0x11 */
static int cs42l63_rate_code(unsigned int rate)
{
	switch (rate) {
	case 8000:	return 1;
	case 11025:	return 2;
	case 12000:	return 3;
	case 16000:	return 4;
	case 22050:	return 5;
	case 24000:	return 6;
	case 32000:	return 7;
	case 44100:	return 8;
	case 48000:	return 9;
	}
	return -EINVAL;
}

static int cs42l63_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct cs42l63 *cs = snd_soc_component_get_drvdata(dai->component);
	int code = cs42l63_rate_code(params_rate(params));

	if (code < 0)
		return code;
	cs->rate_code = code;
	/* still up from the last stream (pmdown_time): change it now */
	if (cs->powered)
		return regmap_update_bits(cs->regmap, CS42L63_SPC, 0x0f, code);
	return 0;
}

/* the only format the ASP is known to take: 16-bit I2S, the SoC's clocks */
static int cs42l63_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	if ((fmt & SND_SOC_DAIFMT_FORMAT_MASK) != SND_SOC_DAIFMT_I2S ||
	    (fmt & SND_SOC_DAIFMT_INV_MASK) != SND_SOC_DAIFMT_NB_NF ||
	    (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) != SND_SOC_DAIFMT_CBC_CFC)
		return -EINVAL;
	return 0;
}

static const struct snd_soc_dai_ops cs42l63_dai_ops = {
	.hw_params = cs42l63_hw_params,
	.set_fmt = cs42l63_set_fmt,
};

static struct snd_soc_dai_driver cs42l63_dai = {
	.name = "cs42l63-asp",
	.playback = {
		.stream_name = "ASP Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |
			 SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.ops = &cs42l63_dai_ops,
};

static const struct snd_soc_component_driver cs42l63_component = {
	.controls = cs42l63_controls,
	.num_controls = ARRAY_SIZE(cs42l63_controls),
	.dapm_widgets = cs42l63_widgets,
	.num_dapm_widgets = ARRAY_SIZE(cs42l63_widgets),
	.dapm_routes = cs42l63_routes,
	.num_dapm_routes = ARRAY_SIZE(cs42l63_routes),
	.use_pmdown_time = 1,
	.endianness = 1,
};

static const struct regmap_config cs42l63_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = CS42L63_MAX_REG,
	.cache_type = REGCACHE_NONE,
	/* a multi-byte transfer needs bit 7 of the address set: keep to one */
	.use_single_read = true,
	.use_single_write = true,
};

static int cs42l63_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct cs42l63 *cs;
	unsigned int id[3], rev;
	int ret, i;

	cs = devm_kzalloc(dev, sizeof(*cs), GFP_KERNEL);
	if (!cs)
		return -ENOMEM;
	cs->dev = dev;
	cs->rate_code = 9;			/* 48 kHz */
	cs->vol[0] = cs->vol[1] = CS42L63_VOL_DEFAULT;
	i2c_set_clientdata(client, cs);

	cs->regmap = devm_regmap_init_i2c(client, &cs42l63_regmap);
	if (IS_ERR(cs->regmap))
		return PTR_ERR(cs->regmap);

	/* the speaker amplifier, off until a stream needs it */
	cs->amp = devm_gpiod_get(dev, "speaker-amp", GPIOD_OUT_LOW);
	if (IS_ERR(cs->amp))
		return dev_err_probe(dev, PTR_ERR(cs->amp), "speaker amplifier GPIO\n");

	for (i = 0; i < 3; i++) {
		ret = regmap_read(cs->regmap, CS42L63_DEVID_AB + i, &id[i]);
		if (ret)
			return dev_err_probe(dev, ret, "no answer\n");
	}
	if (id[0] != 0x42 || id[1] != 0xa6 || (id[2] & 0xf0) != 0x30)
		return dev_err_probe(dev, -ENODEV, "device ID %02x%02x%02x, not a CS42L63\n",
				     id[0], id[1], id[2]);
	ret = regmap_read(cs->regmap, CS42L63_REV_ID, &rev);
	if (ret)
		return ret;
	dev_info(dev, "CS42L63 revision %02x\n", rev);

	return devm_snd_soc_register_component(dev, &cs42l63_component, &cs42l63_dai, 1);
}

static const struct of_device_id cs42l63_of_match[] = {
	{ .compatible = "cirrus,cs42l63" },
	{ }
};
MODULE_DEVICE_TABLE(of, cs42l63_of_match);

static const struct i2c_device_id cs42l63_id[] = {
	{ "cs42l63" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, cs42l63_id);

static struct i2c_driver cs42l63_i2c_driver = {
	.driver = {
		.name = "cs42l63",
		.of_match_table = cs42l63_of_match,
	},
	.id_table = cs42l63_id,
	.probe = cs42l63_i2c_probe,
};
module_i2c_driver(cs42l63_i2c_driver);

MODULE_DESCRIPTION("Cirrus Logic CS42L63 codec, speaker path (iPad 3)");
MODULE_LICENSE("GPL");
