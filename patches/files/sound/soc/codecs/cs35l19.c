// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cirrus Logic CS35L19 boosted class-D speaker amplifier (iPad mini 1).
 *
 * The CS35L19 is the CS35L33's sibling -- the register map matches it reset
 * value for reset value -- with Apple's power-on sequence, which is what this
 * driver runs, taken from iOS 8.4.1's AppleCS35L19Amp (12H321, text
 * 0x80af4de0; docs/research/p105-audio.md):
 *
 *   - the amp needs MCLK (12.000 MHz) before it leaves power-down: without it
 *     INT_STATUS_2 keeps PDN_DONE whatever else is right;
 *   - it watches a KEEPALIVE input and stops when nobody toggles it; iOS
 *     flips the pin every 20 ms, and so does this driver;
 *   - its serial port is a TDM receiver: the audio slot's byte offset and
 *     depth go in RX_AUD (0x36), the sense outputs (0x2d..0x35) stay off.
 *
 * Both amps of the iPad share one reset line and one keep-alive line; the
 * reset is requested non-exclusively, and only the node that carries the
 * keep-alive GPIO toggles it.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

#define CS35L19_DEVID_AB	0x01
#define CS35L19_REV_ID		0x05
#define CS35L19_PWRCTL1		0x06
#define CS35L19_PWRCTL2		0x07
#define CS35L19_CLK_CTL		0x08
#define CS35L19_BST_PEAK	0x09
#define CS35L19_BST_CTL1	0x0b
#define CS35L19_BST_CTL2	0x0c
#define CS35L19_ADSP_CTL	0x0d
#define CS35L19_ADC_CTL		0x0e
#define CS35L19_DAC_CTL		0x0f
#define CS35L19_DIG_VOL		0x10
#define CS35L19_CLASSD_CTL	0x11
#define CS35L19_AMP_CTL		0x12
#define CS35L19_INT_STATUS_1	0x15
#define CS35L19_INT_STATUS_2	0x16
#define CS35L19_TX_VMON		0x2d	/* .. 0x31: V, I, VP, VBST, FLAG */
#define CS35L19_TX_EN1		0x32	/* .. 0x35 */
#define CS35L19_RX_AUD		0x36
#define CS35L19_RX_SPLY		0x37
#define CS35L19_RX_ALIVE	0x38
#define CS35L19_MAX_REG		0x7f

#define CS35L19_VOL_MUTE	0x19	/* what iOS writes to mute */
#define CS35L19_CFG_LEN		20

/* The ADT's speaker-config for P105AP, the same for both amps. */
static const u8 cs35l19_default_cfg[CS35L19_CFG_LEN] = {
	0xff, 0x26, 0x02, 0x70, 0x6a, 0x80, 0x30, 0x1e, 0x02, 0x04,
	0x0c, 0x8a, 0x8a, 0x10, 0x74, 0x03, 0x05, 0x00, 0x00, 0x00,
};

struct cs35l19 {
	struct device *dev;
	struct regmap *regmap;
	struct clk *mclk;
	struct gpio_desc *reset;
	struct gpio_desc *keepalive;
	struct delayed_work alive_work;
	bool alive_level;
	u8 cfg[CS35L19_CFG_LEN];
	unsigned int rev;
	unsigned int rate_code;
	unsigned int slot;		/* TDM slot index (slots are 32 bits) */
};

static void cs35l19_alive(struct work_struct *work)
{
	struct cs35l19 *cs = container_of(to_delayed_work(work), struct cs35l19,
					  alive_work);

	cs->alive_level = !cs->alive_level;
	gpiod_set_value_cansleep(cs->keepalive, cs->alive_level);
	schedule_delayed_work(&cs->alive_work, msecs_to_jiffies(20));
}

static void cs35l19_alive_start(struct cs35l19 *cs)
{
	if (cs->keepalive)
		schedule_delayed_work(&cs->alive_work, 0);
}

static void cs35l19_alive_stop(struct cs35l19 *cs)
{
	if (cs->keepalive)
		cancel_delayed_work_sync(&cs->alive_work);
}

/* AppleCS35L19Amp's power-on (0x80af55d4) and serial port (0x80af5be0). */
static int cs35l19_power_on(struct cs35l19 *cs)
{
	struct regmap *map = cs->regmap;
	const u8 *cfg = cs->cfg;
	unsigned int vol, st1, st2, i;
	int ret;

	ret = regmap_read(map, CS35L19_DIG_VOL, &vol);
	if (ret)
		return ret;

	if (cs->rev >= 0xb0) {
		regmap_write(map, 0x00, 0x99);
		regmap_write(map, 0x45, 0x3f);
		regmap_write(map, 0x46, cfg[16]);
		regmap_write(map, 0x51, 0xff);
		regmap_write(map, 0x59, 0x0a);
		regmap_write(map, 0x6b, 0x20);
		if (cfg[18])
			regmap_write(map, 0x54, cfg[18]);
		regmap_write(map, 0x00, 0x00);
	}
	if (cfg[17]) {
		regmap_write(map, 0x00, 0x99);
		regmap_write(map, 0x66, cfg[17]);
		regmap_write(map, 0x00, 0x00);
	}
	regmap_write(map, CS35L19_BST_CTL1, cfg[1] & 0x3f);
	regmap_update_bits(map, CS35L19_BST_CTL2, 0x03, cfg[2] & 0x03);

	regmap_write(map, CS35L19_PWRCTL1, 0x80);	/* all on but the amp */
	regmap_write(map, CS35L19_PWRCTL2, 0xf8);	/* no V/I/VP/VBST sense, TDM */
	regmap_update_bits(map, CS35L19_CLK_CTL, 0xc0, 0x40);	/* MCLK / 2 */
	regmap_write(map, CS35L19_BST_PEAK, cfg[0]);
	regmap_write(map, CS35L19_DIG_VOL, CS35L19_VOL_MUTE);
	regmap_update_bits(map, CS35L19_DAC_CTL, 0x10, 0);
	regmap_write(map, CS35L19_CLASSD_CTL, 0x1c);	/* calibrate */
	regmap_update_bits(map, CS35L19_ADC_CTL, 0xcf, 0xc4);
	regmap_update_bits(map, CS35L19_AMP_CTL, 0xf0, cfg[3] & 0xf0);
	for (i = 0; i < 4; i++)
		regmap_write(map, 0x3a + i, cfg[12 + i]);
	usleep_range(10000, 11000);
	regmap_write(map, CS35L19_PWRCTL1, 0x00);
	usleep_range(8000, 9000);

	regmap_read(map, CS35L19_INT_STATUS_1, &st1);
	regmap_read(map, CS35L19_INT_STATUS_1, &st1);
	regmap_read(map, CS35L19_INT_STATUS_2, &st2);
	regmap_read(map, CS35L19_INT_STATUS_2, &st2);
	if (st2 & 0x01)
		dev_warn(cs->dev, "still powered down after power-up (status %02x %02x): no MCLK?\n",
			 st1, st2);

	for (i = 0; i < 8; i++)
		regmap_write(map, 0x23 + i, cfg[4 + i]);
	regmap_update_bits(map, 0x29, 0x7f, 0x14);
	regmap_write(map, CS35L19_CLASSD_CTL, 0x14);	/* calibration done */

	/* the serial port: the audio slot only, nothing sent back */
	for (i = 0; i < 5; i++)
		regmap_write(map, CS35L19_TX_VMON + i, 0x80);
	for (i = 0; i < 4; i++)
		regmap_write(map, CS35L19_TX_EN1 + i, 0);
	regmap_write(map, CS35L19_ADSP_CTL, 3);
	regmap_write(map, CS35L19_CLK_CTL, cs->rate_code);
	regmap_write(map, CS35L19_RX_AUD, (cs->slot * 4) | (3 << 5));
	regmap_write(map, CS35L19_RX_SPLY, 0x80);
	regmap_write(map, CS35L19_RX_ALIVE, 0x80);

	return regmap_write(map, CS35L19_DIG_VOL, vol);
}

static void cs35l19_power_off(struct cs35l19 *cs)
{
	struct regmap *map = cs->regmap;
	unsigned int vol;

	if (regmap_read(map, CS35L19_DIG_VOL, &vol))
		return;
	regmap_write(map, CS35L19_DIG_VOL, CS35L19_VOL_MUTE);
	regmap_update_bits(map, CS35L19_PWRCTL1, 0x80, 0x80);
	regmap_write(map, CS35L19_PWRCTL2, 0xfe);
	usleep_range(1000, 2000);
	regmap_write(map, CS35L19_PWRCTL1, 0x87);
	usleep_range(4000, 5000);
	regmap_write(map, CS35L19_DIG_VOL, vol);
}

static int cs35l19_amp_event(struct snd_soc_dapm_widget *w,
			     struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct cs35l19 *cs = snd_soc_component_get_drvdata(component);
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
		cs35l19_alive_start(cs);
		ret = cs35l19_power_on(cs);
		if (ret) {
			cs35l19_alive_stop(cs);
			clk_disable_unprepare(cs->mclk);
		}
		return ret;
	case SND_SOC_DAPM_POST_PMD:
		cs35l19_power_off(cs);
		cs35l19_alive_stop(cs);
		clk_disable_unprepare(cs->mclk);
		return 0;
	}
	return 0;
}

/* 0.5 dB steps around 0 dB: 0x34 is -102 dB, 0x00 0 dB, 0x18 +12 dB */
static const DECLARE_TLV_DB_SCALE(cs35l19_vol_tlv, -10200, 50, 0);

static const struct snd_kcontrol_new cs35l19_controls[] = {
	SOC_SINGLE_SX_TLV("Speaker Volume", CS35L19_DIG_VOL, 0, 0x34, 0xe4,
			  cs35l19_vol_tlv),
};

static const struct snd_soc_dapm_widget cs35l19_widgets[] = {
	SND_SOC_DAPM_AIF_IN("SDIN", "Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_OUT_DRV_E("AMP", SND_SOC_NOPM, 0, 0, NULL, 0,
			       cs35l19_amp_event,
			       SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_OUTPUT("SPK"),
};

static const struct snd_soc_dapm_route cs35l19_routes[] = {
	{ "AMP", NULL, "SDIN" },
	{ "SPK", NULL, "AMP" },
};

static int cs35l19_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct cs35l19 *cs = snd_soc_component_get_drvdata(dai->component);

	/* the rate codes iOS writes to CLK_CTL, with MCLK = 12 MHz */
	switch (params_rate(params)) {
	case 48000:
		cs->rate_code = 0x49;
		break;
	case 44100:
		cs->rate_code = 0x4b;
		break;
	case 32000:
		cs->rate_code = 0x4d;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int cs35l19_set_tdm_slot(struct snd_soc_dai *dai, unsigned int tx_mask,
				unsigned int rx_mask, int slots, int slot_width)
{
	struct cs35l19 *cs = snd_soc_component_get_drvdata(dai->component);

	if (slot_width != 32 || !rx_mask)
		return -EINVAL;
	cs->slot = __ffs(rx_mask);
	return 0;
}

static const struct snd_soc_dai_ops cs35l19_dai_ops = {
	.hw_params = cs35l19_hw_params,
	.set_tdm_slot = cs35l19_set_tdm_slot,
};

static struct snd_soc_dai_driver cs35l19_dai = {
	.name = "cs35l19",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |
			 SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S32_LE,
	},
	.ops = &cs35l19_dai_ops,
};

static const struct snd_soc_component_driver cs35l19_component = {
	.controls = cs35l19_controls,
	.num_controls = ARRAY_SIZE(cs35l19_controls),
	.dapm_widgets = cs35l19_widgets,
	.num_dapm_widgets = ARRAY_SIZE(cs35l19_widgets),
	.dapm_routes = cs35l19_routes,
	.num_dapm_routes = ARRAY_SIZE(cs35l19_routes),
	.use_pmdown_time = 1,
	.endianness = 1,
};

static const struct regmap_config cs35l19_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = CS35L19_MAX_REG,
	.cache_type = REGCACHE_NONE,
	/* the amp does not step the register address within a transfer */
	.use_single_read = true,
	.use_single_write = true,
};

static int cs35l19_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct cs35l19 *cs;
	u8 id[3];
	int ret, n;

	cs = devm_kzalloc(dev, sizeof(*cs), GFP_KERNEL);
	if (!cs)
		return -ENOMEM;
	cs->dev = dev;
	cs->rate_code = 0x49;
	cs->slot = client->addr & 1;
	INIT_DELAYED_WORK(&cs->alive_work, cs35l19_alive);
	i2c_set_clientdata(client, cs);

	cs->regmap = devm_regmap_init_i2c(client, &cs35l19_regmap);
	if (IS_ERR(cs->regmap))
		return PTR_ERR(cs->regmap);

	/* shared by both amps: out of reset, and left there */
	cs->reset = devm_gpiod_get_optional(dev, "reset",
					    GPIOD_OUT_LOW | GPIOD_FLAGS_BIT_NONEXCLUSIVE);
	if (IS_ERR(cs->reset))
		return dev_err_probe(dev, PTR_ERR(cs->reset), "reset GPIO\n");
	cs->keepalive = devm_gpiod_get_optional(dev, "cirrus,keepalive",
						GPIOD_OUT_LOW);
	if (IS_ERR(cs->keepalive))
		return dev_err_probe(dev, PTR_ERR(cs->keepalive), "keep-alive GPIO\n");
	if (cs->reset)
		usleep_range(2000, 3000);

	memcpy(cs->cfg, cs35l19_default_cfg, sizeof(cs->cfg));
	n = of_property_count_u8_elems(dev->of_node, "apple,speaker-config");
	if (n == CS35L19_CFG_LEN)
		of_property_read_u8_array(dev->of_node, "apple,speaker-config",
					  cs->cfg, CS35L19_CFG_LEN);

	ret = regmap_bulk_read(cs->regmap, CS35L19_DEVID_AB, id, 3);
	if (ret)
		return dev_err_probe(dev, ret, "no answer\n");
	if (id[0] != 0x35 || id[1] != 0xa1 || (id[2] & 0xf0) != 0x90)
		return dev_err_probe(dev, -ENODEV, "device ID %02x%02x%02x, not a CS35L19\n",
				     id[0], id[1], id[2]);
	ret = regmap_read(cs->regmap, CS35L19_REV_ID, &cs->rev);
	if (ret)
		return ret;
	dev_info(dev, "CS35L19 revision %02x\n", cs->rev);

	return devm_snd_soc_register_component(dev, &cs35l19_component,
					       &cs35l19_dai, 1);
}

static void cs35l19_i2c_remove(struct i2c_client *client)
{
	struct cs35l19 *cs = i2c_get_clientdata(client);

	cancel_delayed_work_sync(&cs->alive_work);
}

static const struct of_device_id cs35l19_of_match[] = {
	{ .compatible = "cirrus,cs35l19" },
	{ }
};
MODULE_DEVICE_TABLE(of, cs35l19_of_match);

static const struct i2c_device_id cs35l19_id[] = {
	{ "cs35l19" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, cs35l19_id);

static struct i2c_driver cs35l19_i2c_driver = {
	.driver = {
		.name = "cs35l19",
		.of_match_table = cs35l19_of_match,
	},
	.id_table = cs35l19_id,
	.probe = cs35l19_i2c_probe,
	.remove = cs35l19_i2c_remove,
};
module_i2c_driver(cs35l19_i2c_driver);

MODULE_DESCRIPTION("Cirrus Logic CS35L19 speaker amplifier (iPad mini 1)");
MODULE_LICENSE("GPL");
