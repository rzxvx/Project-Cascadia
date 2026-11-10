// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cirrus Logic CS42L63, as the iPad 2 (K93) has it: the codec behind the A5's
 * I2S0, its line out 2 feeding the speaker's amplifier.
 *
 * There is no datasheet for the part (a CS42L73 sibling, device ID 42 a6 3x).
 * This driver does what iOS's AppleCS42L63Audio does, step for step: its
 * sequences were taken by running iOS's own code in an emulator
 * (docs/research/k93-audio.md, tools/audio/k93-cs42l63-sequences.py) and are
 * compiled in from cs42l63-seq.h, which tools/audio/k93-seq2c.py generates --
 * the same steps tools/audio/k93-seq2sh.py turns into i2cset lines.
 *
 *   probe          MCLK on (the codec needs it to leave reset), then iOS's
 *                  cold init: reset 10 ms, the test-page writes, every block
 *                  configured and powered, outputs muted
 *   hw_params      the rate code (48 or 44.1 kHz)
 *   unmute         iOS's speaker_start: line out 2 on, the mixer's
 *                  attenuation and the speaker volume, then the amplifier's
 *                  enable (SoC GPIO 39) and 50 ms
 *   mute           speaker_stop: the amplifier off first, then the same
 *                  registers back to their muted values
 *
 * Two controls, both in iOS's units (0-62): "Master Playback Volume" is the
 * mixer attenuation (0x45 = 62 - v, 0x49 = 0x40 | (62 - v)) and "Speaker
 * Playback Volume" the line-out level (0x21 = 0x22 = 0x41 + v), as iOS's
 * master_volume_* and speaker_volume_* sequences write them.  Headphones,
 * the TS3A8235 jack switch and the microphones are not done yet.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

enum { CS_W, CS_U, CS_R, CS_SLEEP, CS_RESET, CS_SPK, CS_MCLK };

struct cs42l63_step {
	u8 op, reg, mask, val;
};

#define CS42L63_W(r, v)		{ CS_W, (r), 0xff, (v) }
#define CS42L63_U(r, m, v)	{ CS_U, (r), (m), (v) }
#define CS42L63_R(r)		{ CS_R, (r), 0, 0 }
#define CS42L63_SLEEP(ms)	{ CS_SLEEP, 0, 0, (ms) }
#define CS42L63_RESET(on)	{ CS_RESET, 0, 0, (on) }
#define CS42L63_SPK(on)		{ CS_SPK, 0, 0, (on) }
#define CS42L63_MCLK(on)	{ CS_MCLK, 0, 0, (on) }

#include "cs42l63-seq.h"

#define CS42L63_VOL_MAX		62

struct cs42l63 {
	struct i2c_client *i2c;
	struct device *dev;
	struct gpio_desc *reset;	/* active low: 1 = held in reset */
	struct gpio_desc *spk;		/* the amplifier's enable: 1 = plays */
	struct clk *mclk;
	bool mclk_on;
	struct mutex lock;
	bool playing;
	unsigned int master, speaker;	/* 0..62, iOS's units */
};

static int cs42l63_run(struct cs42l63 *c, const struct cs42l63_step *s, unsigned int n)
{
	unsigned int i;
	int v, ret = 0;

	for (i = 0; i < n && !ret; i++, s++) {
		switch (s->op) {
		case CS_W:
			ret = i2c_smbus_write_byte_data(c->i2c, s->reg, s->val);
			break;
		case CS_U:
			v = i2c_smbus_read_byte_data(c->i2c, s->reg);
			if (v < 0) {
				ret = v;
				break;
			}
			ret = i2c_smbus_write_byte_data(c->i2c, s->reg, (v & ~s->mask) | s->val);
			break;
		case CS_R:
			v = i2c_smbus_read_byte_data(c->i2c, s->reg);
			if (v < 0)
				ret = v;
			else
				dev_dbg(c->dev, "reg 0x%02x = 0x%02x\n", s->reg, v);
			break;
		case CS_SLEEP:
			msleep(s->val);
			break;
		case CS_RESET:
			gpiod_set_value_cansleep(c->reset, s->val);
			break;
		case CS_SPK:
			if (c->spk)
				gpiod_set_value_cansleep(c->spk, s->val);
			break;
		case CS_MCLK:
			if (s->val && !c->mclk_on) {
				ret = clk_prepare_enable(c->mclk);
				c->mclk_on = !ret;
			} else if (!s->val && c->mclk_on) {
				clk_disable_unprepare(c->mclk);
				c->mclk_on = false;
			}
			break;
		}
	}
	if (ret)
		dev_err(c->dev, "step %u (op %u, reg 0x%02x) failed: %d\n", i - 1,
			s[-1].op, s[-1].reg, ret);
	return ret;
}

#define RUN(c, seq)	cs42l63_run((c), cs42l63_##seq, ARRAY_SIZE(cs42l63_##seq))

/* the volumes as iOS writes them while the speaker plays */
static int cs42l63_set_volumes(struct cs42l63 *c)
{
	unsigned int att = CS42L63_VOL_MAX - c->master;
	int ret;

	ret = i2c_smbus_write_byte_data(c->i2c, 0x45, att);
	ret = ret ?: i2c_smbus_write_byte_data(c->i2c, 0x49, 0x40 | att);
	ret = ret ?: i2c_smbus_write_byte_data(c->i2c, 0x21, 0x41 + c->speaker);
	ret = ret ?: i2c_smbus_write_byte_data(c->i2c, 0x22, 0x41 + c->speaker);
	return ret;
}

/* --- controls ------------------------------------------------------------- */

static int cs42l63_vol_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	ui->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	ui->count = 1;
	ui->value.integer.min = 0;
	ui->value.integer.max = CS42L63_VOL_MAX;
	return 0;
}

static unsigned int *cs42l63_vol_field(struct cs42l63 *c, struct snd_kcontrol *kc)
{
	return kc->private_value ? &c->speaker : &c->master;
}

static int cs42l63_vol_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct snd_soc_component *comp = snd_kcontrol_chip(kc);
	struct cs42l63 *c = snd_soc_component_get_drvdata(comp);

	mutex_lock(&c->lock);
	uc->value.integer.value[0] = *cs42l63_vol_field(c, kc);
	mutex_unlock(&c->lock);
	return 0;
}

static int cs42l63_vol_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct snd_soc_component *comp = snd_kcontrol_chip(kc);
	struct cs42l63 *c = snd_soc_component_get_drvdata(comp);
	long v = uc->value.integer.value[0];
	unsigned int *f;
	int ret = 0;

	if (v < 0 || v > CS42L63_VOL_MAX)
		return -EINVAL;
	mutex_lock(&c->lock);
	f = cs42l63_vol_field(c, kc);
	if (*f == v) {
		mutex_unlock(&c->lock);
		return 0;
	}
	*f = v;
	if (c->playing)
		ret = cs42l63_set_volumes(c);
	mutex_unlock(&c->lock);
	return ret ?: 1;
}

static const struct snd_kcontrol_new cs42l63_controls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Master Playback Volume",
		.info = cs42l63_vol_info,
		.get = cs42l63_vol_get,
		.put = cs42l63_vol_put,
		.private_value = 0,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Speaker Playback Volume",
		.info = cs42l63_vol_info,
		.get = cs42l63_vol_get,
		.put = cs42l63_vol_put,
		.private_value = 1,
	},
};

/* --- the DAI ---------------------------------------------------------------- */

static int cs42l63_hw_params(struct snd_pcm_substream *ss, struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct cs42l63 *c = snd_soc_component_get_drvdata(dai->component);
	int ret;

	mutex_lock(&c->lock);
	switch (params_rate(params)) {
	case 48000:
		ret = RUN(c, rate_48000);
		break;
	case 44100:
		ret = RUN(c, rate_44100);
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&c->lock);
	return ret;
}

static int cs42l63_mute_stream(struct snd_soc_dai *dai, int mute, int stream)
{
	struct cs42l63 *c = snd_soc_component_get_drvdata(dai->component);
	int ret = 0;

	if (stream != SNDRV_PCM_STREAM_PLAYBACK)
		return 0;
	mutex_lock(&c->lock);
	if (!mute && !c->playing) {
		ret = RUN(c, speaker_start);
		/* speaker_start leaves both at iOS's maximum; ours, before the
		 * amplifier has settled */
		ret = ret ?: cs42l63_set_volumes(c);
		c->playing = !ret;
	} else if (mute && c->playing) {
		ret = RUN(c, speaker_stop);
		c->playing = false;
	}
	mutex_unlock(&c->lock);
	return ret;
}

static const struct snd_soc_dai_ops cs42l63_dai_ops = {
	.hw_params = cs42l63_hw_params,
	.mute_stream = cs42l63_mute_stream,
	.no_capture_mute = 1,
};

static struct snd_soc_dai_driver cs42l63_dai = {
	.name = "cs42l63",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.ops = &cs42l63_dai_ops,
};

/* --- component -------------------------------------------------------------- */

static int cs42l63_component_probe(struct snd_soc_component *comp)
{
	struct cs42l63 *c = snd_soc_component_get_drvdata(comp);
	int id[4], i, ret;

	mutex_lock(&c->lock);
	ret = RUN(c, init_cold);
	mutex_unlock(&c->lock);
	if (ret)
		return ret;
	/* the ID can only be read out of reset and with MCLK: after init_cold */
	for (i = 0; i < 3; i++)
		id[i] = i2c_smbus_read_byte_data(c->i2c, 1 + i);	/* 0x01-0x03 */
	id[3] = i2c_smbus_read_byte_data(c->i2c, 0x05);		/* revision */
	if (id[0] != 0x42 || id[1] != 0xa6)
		dev_warn(c->dev, "device ID %02x %02x %02x, not a CS42L63's 42 a6 3x\n",
			 id[0] & 0xff, id[1] & 0xff, id[2] & 0xff);
	else
		dev_info(c->dev, "CS42L63, device ID %02x %02x %02x, revision %02x\n",
			 id[0], id[1], id[2], id[3] & 0xff);
	return 0;
}

static void cs42l63_component_remove(struct snd_soc_component *comp)
{
	struct cs42l63 *c = snd_soc_component_get_drvdata(comp);

	mutex_lock(&c->lock);
	if (c->playing)
		RUN(c, speaker_stop);
	c->playing = false;
	RUN(c, power_down);
	gpiod_set_value_cansleep(c->reset, 1);
	if (c->mclk_on)
		clk_disable_unprepare(c->mclk);
	c->mclk_on = false;
	mutex_unlock(&c->lock);
}

static const struct snd_soc_component_driver cs42l63_component = {
	.probe = cs42l63_component_probe,
	.remove = cs42l63_component_remove,
	.controls = cs42l63_controls,
	.num_controls = ARRAY_SIZE(cs42l63_controls),
	.endianness = 1,
};

static int cs42l63_i2c_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	struct cs42l63 *c;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->i2c = i2c;
	c->dev = dev;
	mutex_init(&c->lock);
	i2c_set_clientdata(i2c, c);
	/* iOS's defaults: both at the top -- loud.  The master a little lower */
	c->speaker = CS42L63_VOL_MAX;
	c->master = 48;

	c->mclk = devm_clk_get(dev, "mclk");
	if (IS_ERR(c->mclk))
		return dev_err_probe(dev, PTR_ERR(c->mclk), "no MCLK\n");
	/* held in reset until the cold init lets it go, as iOS's start() does */
	c->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(c->reset))
		return dev_err_probe(dev, PTR_ERR(c->reset), "no reset GPIO\n");
	c->spk = devm_gpiod_get_optional(dev, "speaker-enable", GPIOD_OUT_LOW);
	if (IS_ERR(c->spk))
		return dev_err_probe(dev, PTR_ERR(c->spk), "no speaker-enable GPIO\n");

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

static struct i2c_driver cs42l63_driver = {
	.driver = {
		.name = "cs42l63",
		.of_match_table = cs42l63_of_match,
	},
	.probe = cs42l63_i2c_probe,
	.id_table = cs42l63_id,
};
module_i2c_driver(cs42l63_driver);

MODULE_DESCRIPTION("Cirrus Logic CS42L63 codec (iPad 2), iOS's sequences");
MODULE_LICENSE("GPL");
