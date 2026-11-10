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
 * Volumes in iOS's units (0-62): "Master Playback Volume" is the mixer
 * attenuation of the output in use (speaker: 0x45 = 62 - v, 0x49 = 0x40 |
 * (62 - v); headphones: 0x3d, 0x41 the same), "Speaker Playback Volume" and
 * "Headphone Playback Volume" the outputs' analog levels (0x21/0x22,
 * 0x23/0x25 = 0x41 + v), as iOS's master_volume_*, speaker_volume_* and
 * hp_*_volume_* sequences write them.  iOS's start sequences open an output
 * at its maximum; here every such write carries the volume set instead, so
 * nothing is ever louder than asked, not even for a millisecond.
 *
 * "Headphone Playback Switch" sends playback to the headphones (hp_start)
 * instead of the speaker, at once if a stream plays.  Capture runs iOS's
 * input_internal_mic or input_headset_mic ("Capture Source"), input_stop
 * after, and "Capture Volume" is iOS's input volume (control 0x16f) through
 * iOS's own mapping for that source (cs42l63_gain_*: a boost, the analog
 * stage, the digital volume), on one dB scale for both: 0-128 = -96 to +32
 * dB in 1 dB steps, iOS's default the top.  The dB are iOS's own (its
 * control ranges: the internal mic 1 dB a step from -96 dB, the headset's
 * 0.5 dB from -92 dB, hence two tables); on the headset, speech measured
 * them right within 1 dB.  iOS records a mic -- one, on the ASP's left
 * channel -- in mono; here it is also mixed into the right one (0x58 or
 * 0x5b), so a stereo recording has it twice.
 *
 * The jack: the Mikey block's register 0x77 bit 7 is set while a plug is in
 * (0x7b latches bit 6 on insertion, bit 5 on removal, and clears when read),
 * measured on the iPad.  Its interrupt line (SoC GPIO 15) stays masked as
 * iOS's init leaves it, so the jack is polled, twice a second.  When a plug
 * goes in, this does what iOS's AppleCSCodecMikey does for a headset without
 * Apple's remote (docs/research/k93-audio.md):
 *   - it measures the plug's mic contact under both wirings of the TS3A8235
 *     (headset-switch in the DT: the jack's ground and mic contacts), CTIA
 *     (0x42) and OMTP (0x43): the bias on (0x74 = 0x06), the comparators'
 *     reference at 0x17 and 0x36 (0x75), the answer in 0x78 -- open, mic,
 *     reverse-bias or short;
 *   - keeps the wiring iOS would (OMTP only if CTIA reads reversed or short
 *     and OMTP does not);
 *   - and if a mic is there, leaves its bias on as iOS's MicOpen state does
 *     (0x75 = 0x3c, 0x74 = 0x1e), behind the switch's pop guard; without the
 *     bias that mic records exact zeros (measured on the iPad).
 * Then playback moves to the headphones and, with a mic, capture to it;
 * both move back when the plug comes out (the bias off, the switch at 0x00:
 * as iBoot leaves it, it passes nothing until it is written).  "Headphone
 * Jack" and "Headset Mic Jack" tell userspace, the moved controls notified.
 * Apple's remote (Mikey's own protocol) and a headset's buttons are not done.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/workqueue.h>
#include <sound/jack.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

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
#define CS42L63_GAIN_MAX	(ARRAY_SIZE(cs42l63_gain_internal) - 1)
static_assert(ARRAY_SIZE(cs42l63_gain_internal) == ARRAY_SIZE(cs42l63_gain_headset));

struct cs42l63 {
	struct i2c_client *i2c;
	struct device *dev;
	struct gpio_desc *reset;	/* active low: 1 = held in reset */
	struct gpio_desc *spk;		/* the amplifier's enable: 1 = plays */
	struct clk *mclk;
	bool mclk_on;
	struct mutex lock;
	bool playing, recording;
	bool hp;			/* playback to the headphones, not the speaker */
	bool headset_mic;		/* capture from the headset's mic, not the built-in one */
	unsigned int master, speaker, headphone;	/* 0..62, iOS's units */
	unsigned int capture;		/* the input volume, 0..128 = -96..+32 dB */

	struct i2c_client *hsw;		/* the TS3A8235, if the DT names it */
	u8 hsw_state;			/* what it was last set to */
	struct snd_soc_component *comp;
	struct snd_soc_jack jack;
	struct delayed_work jack_work;
	bool jack_in;
	bool mic;			/* the plug in has a microphone */
	bool bias;			/* the Mikey's mic bias is on */
};

/*
 * The TS3A8235: iOS's setState writes only register 2.  Its states 1 and 2
 * are the two wirings of a 4-pole plug; bit 6 is clear while iOS measures the
 * mic contact and in its pop guard (its "slow" states), set otherwise.
 */
#define TS3A8235_STATE		0x02
#define TS3A8235_CTIA		0x42	/* state 1: CTIA headsets (Apple's), headphones */
#define TS3A8235_OMTP		0x43	/* state 2: the OMTP wiring */
#define TS3A8235_GUARD		0x06	/* state 3, slow: while the mic bias comes on */
#define TS3A8235_NORMAL		0x40	/* clear in the slow states */
#define TS3A8235_NONE		0x00
#define TS3A8235_RAMP_MS	50	/* the ADT's slow-ramp-delay */

/* the Mikey block, as iOS's AppleCSCodecMikey drives it (0x70 + its register) */
#define MIKEY_CTL		0x70	/* 0x01 in the modes used here */
#define MIKEY_CTL2		0x71	/* bits 7:6 set in them */
#define MIKEY_MODE		0x74	/* 0x03 bias off, 0x06 measuring, 0x1e a mic in use */
#define MIKEY_REF		0x75	/* bits 5:0: the comparators' reference */
#define MIKEY_SENSE		0x78	/* bit 0 the mic contact shorted, bit 1 above the reference */
#define MIKEY_MODE_OFF		0x03
#define MIKEY_MODE_MEASURE	0x06
#define MIKEY_MODE_MIC		0x1e

/* iOS's names for what the mic contact reads (0x8091bd14) */
enum { LINE_OPEN, LINE_MIC, LINE_REVERSE, LINE_SHORT };
static const char * const cs42l63_line[] = { "open", "mic", "reverse-bias", "short" };

#define CS42L63_JACK_POLL_MS	500
#define CS42L63_JACK_SETTLE_MS	200	/* a plug seen moving is read again after this */
#define CS42L63_JACK_REG	0x77	/* Mikey register 7: bit 7, a plug is in */
#define CS42L63_JACK_IN		0x80

/*
 * The value to write instead of what a sequence wants: an output's analog
 * level or the active mixer entry, unless the sequence mutes it (0x40 for a
 * level, 0x3f / 0x7f for a mixer entry), is the volume that is set.
 */
static u8 cs42l63_vol_subst(struct cs42l63 *c, u8 reg, u8 val)
{
	u8 att = CS42L63_VOL_MAX - c->master;

	switch (reg) {
	case 0x21: case 0x22:			/* speaker (line out 2) level */
		return val == 0x40 ? val : 0x41 + c->speaker;
	case 0x23: case 0x25:			/* headphone A / B level */
		return val == 0x40 ? val : 0x41 + c->headphone;
	case 0x3d: case 0x45:			/* ASP L -> HP A / speaker A */
		return val == 0x3f ? val : att;
	case 0x41: case 0x49:			/* ASP R -> HP B / speaker B */
		return val == 0x7f ? val : 0x40 | att;
	}
	return val;
}

static int cs42l63_run(struct cs42l63 *c, const struct cs42l63_step *s, unsigned int n)
{
	unsigned int i;
	int v, ret = 0;

	for (i = 0; i < n && !ret; i++, s++) {
		switch (s->op) {
		case CS_W:
			ret = i2c_smbus_write_byte_data(c->i2c, s->reg,
							 cs42l63_vol_subst(c, s->reg, s->val));
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

/* the volumes of the output in use, as iOS writes them while it plays */
static int cs42l63_set_volumes(struct cs42l63 *c)
{
	unsigned int att = CS42L63_VOL_MAX - c->master;
	unsigned int lvl = 0x41 + (c->hp ? c->headphone : c->speaker);
	u8 m0 = c->hp ? 0x3d : 0x45, m1 = c->hp ? 0x41 : 0x49;
	u8 l0 = c->hp ? 0x23 : 0x21, l1 = c->hp ? 0x25 : 0x22;
	int ret;

	ret = i2c_smbus_write_byte_data(c->i2c, m0, att);
	ret = ret ?: i2c_smbus_write_byte_data(c->i2c, m1, 0x40 | att);
	ret = ret ?: i2c_smbus_write_byte_data(c->i2c, l0, lvl);
	ret = ret ?: i2c_smbus_write_byte_data(c->i2c, l1, lvl);
	return ret;
}

/* the output in use on and off (iOS's routing: the other one stays muted) */
static int cs42l63_out_start(struct cs42l63 *c)
{
	return c->hp ? RUN(c, hp_start) : RUN(c, speaker_start);
}

static int cs42l63_out_stop(struct cs42l63 *c)
{
	return c->hp ? RUN(c, hp_stop) : RUN(c, speaker_stop);
}

/*
 * iOS's input volume for the source in use, and the mic in both channels: iOS
 * sends the internal mic's path A (0x54) or the headset's path B (0x57) to the
 * ASP's left channel; the same path's entry for the right one is 0x58 or 0x5b.
 */
static int cs42l63_set_gain(struct cs42l63 *c)
{
	const u8 *g = (c->headset_mic ? cs42l63_gain_headset : cs42l63_gain_internal)[c->capture];
	int v = i2c_smbus_read_byte_data(c->i2c, 0x14), ret, i;

	if (v < 0)
		return v;
	ret = i2c_smbus_write_byte_data(c->i2c, 0x14, (v & ~0x07) | g[0]);
	for (i = 0; i < 3 && !ret; i++)
		ret = i2c_smbus_write_byte_data(c->i2c, 0x16 + i, g[1]);
	for (i = 0; i < 3 && !ret; i++)
		ret = i2c_smbus_write_byte_data(c->i2c, 0x19 + i, g[2]);
	if (!ret)
		ret = i2c_smbus_write_byte_data(c->i2c, c->headset_mic ? 0x5b : 0x58, 0x00);
	return ret;
}

/* playback to the headphones or the speaker, moved at once while a stream plays */
static int cs42l63_set_hp(struct cs42l63 *c, bool hp)
{
	int ret = 0;

	if (c->hp == hp)
		return 0;
	if (c->playing) {
		ret = cs42l63_out_stop(c);
		c->hp = hp;
		ret = ret ?: cs42l63_out_start(c);
	} else {
		c->hp = hp;
	}
	return ret;
}

static int cs42l63_in_start(struct cs42l63 *c)
{
	int ret = c->headset_mic ? RUN(c, input_headset_mic) : RUN(c, input_internal_mic);

	return ret ?: cs42l63_set_gain(c);
}

/* capture from the headset's mic or the built-in one, moved at once while recording */
static int cs42l63_set_source(struct cs42l63 *c, bool headset)
{
	int ret = 0;

	if (c->headset_mic == headset)
		return 0;
	if (c->recording) {
		ret = RUN(c, input_stop);
		c->headset_mic = headset;
		ret = ret ?: cs42l63_in_start(c);
	} else {
		c->headset_mic = headset;
	}
	return ret;
}

/* --- the Mikey block and the headset switch ----------------------------------- */

static int cs42l63_upd(struct cs42l63 *c, u8 reg, u8 mask, u8 val)
{
	int v = i2c_smbus_read_byte_data(c->i2c, reg);

	return v < 0 ? v : i2c_smbus_write_byte_data(c->i2c, reg, (v & ~mask) | val);
}

static int cs42l63_hsw_set(struct cs42l63 *c, u8 state)
{
	c->hsw_state = state;
	return c->hsw ? i2c_smbus_write_byte_data(c->hsw, TS3A8235_STATE, state) : 0;
}

/*
 * One of iOS's Mikey modes as the K93 runs them (AppleCSCodecMikey 0x8091c464,
 * modes 0/3, 4 and 8): 0x70 = 0x01, 0x71 bits 7:6 set, 0x74 = the mode, then
 * its settling time.  The bias coming on (0x74 bits 2:1 other than 01) goes
 * behind the pop guard (0x8091c63c, headphone-pop-workaround = 2): the switch
 * in its guard state for the slow-ramp delay while 0x74 changes -- unless it
 * is in a slow state already, measuring.
 */
static int cs42l63_mikey_mode(struct cs42l63 *c, u8 mode)
{
	bool on = (mode & 6) != 2;
	u8 state = c->hsw_state;
	int ret;

	ret = i2c_smbus_write_byte_data(c->i2c, MIKEY_CTL, 0x01);
	ret = ret ?: cs42l63_upd(c, MIKEY_CTL2, 0xc0, 0xc0);
	if (!ret && on && !c->bias && c->hsw && (state & TS3A8235_NORMAL)) {
		ret = cs42l63_hsw_set(c, TS3A8235_GUARD);
		ret = ret ?: i2c_smbus_write_byte_data(c->i2c, MIKEY_MODE, mode);
		msleep(TS3A8235_RAMP_MS);
		ret = cs42l63_hsw_set(c, state) ?: ret;
	}
	ret = ret ?: i2c_smbus_write_byte_data(c->i2c, MIKEY_MODE, mode);
	if (!ret)
		c->bias = on;
	msleep(mode & 4 ? 35 : mode & 1 ? 10 : 15);
	return ret;
}

/*
 * What the mic contact reads in the switch's present wiring (iOS's
 * 0x8091bc00 and 0x8091bd14): the bias on, reference 0x17 -- shorted, or
 * reversed (above it) -- then reference 0x36: a mic above it, else open.
 */
static int cs42l63_mikey_sense(struct cs42l63 *c)
{
	int v, line, ret;

	ret = cs42l63_upd(c, MIKEY_REF, 0x3f, 0x17);
	ret = ret ?: cs42l63_mikey_mode(c, MIKEY_MODE_MEASURE);
	if (ret)
		return ret;
	msleep(TS3A8235_RAMP_MS);
	v = i2c_smbus_read_byte_data(c->i2c, MIKEY_SENSE);
	if (v < 0) {
		line = v;
	} else if (v & 1) {
		line = LINE_SHORT;
	} else if (v & 2) {
		line = LINE_REVERSE;
	} else {
		line = cs42l63_upd(c, MIKEY_REF, 0x3f, 0x36);
		usleep_range(1000, 2000);
		v = line ?: i2c_smbus_read_byte_data(c->i2c, MIKEY_SENSE);
		if (v < 0)
			line = v;
		else
			line = (v & 2) ? LINE_MIC : LINE_OPEN;
	}
	ret = cs42l63_mikey_mode(c, MIKEY_MODE_OFF);
	msleep(TS3A8235_RAMP_MS);
	return line < 0 ? line : ret ?: line;
}

/* a plug went in: its wiring and its mic, as iOS's detection finds them */
static void cs42l63_plug_in(struct cs42l63 *c)
{
	static const u8 wiring[2] = { TS3A8235_CTIA, TS3A8235_OMTP };
	int r[2] = { LINE_OPEN, LINE_OPEN }, i, ret;

	if (!c->hsw) {
		c->mic = false;
		return;
	}
	ret = cs42l63_mikey_mode(c, MIKEY_MODE_OFF);
	for (i = 0; i < 2 && !ret; i++) {
		ret = cs42l63_hsw_set(c, wiring[i] & ~TS3A8235_NORMAL);
		ret = ret ?: cs42l63_mikey_sense(c);
		if (ret >= 0) {
			r[i] = ret;
			ret = 0;
		}
	}
	if (ret)
		dev_warn(c->dev, "could not measure the plug's mic contact: %d\n", ret);
	/* iOS's choice: OMTP if CTIA reads reversed or short and OMTP does not */
	i = r[0] >= LINE_REVERSE && r[1] <= LINE_MIC;
	c->mic = !ret && r[i] == LINE_MIC;
	if (cs42l63_hsw_set(c, wiring[i]))
		dev_warn(c->dev, "headset switch not set\n");
	if (c->mic) {
		ret = cs42l63_upd(c, MIKEY_REF, 0x3f, 0x3c);		/* MicOpen */
		ret = ret ?: cs42l63_mikey_mode(c, MIKEY_MODE_MIC);
	} else {
		ret = cs42l63_upd(c, MIKEY_REF, 0x3f, 0x36);		/* HeadphonesOnly */
		ret = ret ?: cs42l63_mikey_mode(c, MIKEY_MODE_OFF);
	}
	if (ret)
		dev_warn(c->dev, "could not set the mic bias: %d\n", ret);
	dev_info(c->dev, "plug in: the mic contact reads %s (CTIA), %s (OMTP): %s wiring, %s\n",
		 cs42l63_line[r[0]], cs42l63_line[r[1]], i ? "OMTP" : "CTIA",
		 c->mic ? "a headset: its mic biased" : "headphones");
}

/* the plug came out: the bias off, the switch open */
static void cs42l63_plug_out(struct cs42l63 *c)
{
	int ret;

	ret = cs42l63_mikey_mode(c, MIKEY_MODE_OFF);
	ret = ret ?: cs42l63_upd(c, MIKEY_REF, 0x3f, 0x36);
	if (c->hsw)
		ret = cs42l63_hsw_set(c, TS3A8235_NONE) ?: ret;
	if (ret)
		dev_warn(c->dev, "could not release the jack: %d\n", ret);
	c->mic = false;
	dev_info(c->dev, "plug out\n");
}

/* --- controls ------------------------------------------------------------- */

static unsigned int cs42l63_vol_max(struct snd_kcontrol *kc)
{
	return kc->private_value == 3 ? CS42L63_GAIN_MAX : CS42L63_VOL_MAX;
}

static int cs42l63_vol_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	ui->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	ui->count = 1;
	ui->value.integer.min = 0;
	ui->value.integer.max = cs42l63_vol_max(kc);
	return 0;
}

static unsigned int *cs42l63_vol_field(struct cs42l63 *c, struct snd_kcontrol *kc)
{
	switch (kc->private_value) {
	case 1:
		return &c->speaker;
	case 2:
		return &c->headphone;
	case 3:
		return &c->capture;
	}
	return &c->master;
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

	if (v < 0 || v > cs42l63_vol_max(kc))
		return -EINVAL;
	mutex_lock(&c->lock);
	f = cs42l63_vol_field(c, kc);
	if (*f == v) {
		mutex_unlock(&c->lock);
		return 0;
	}
	*f = v;
	if (kc->private_value == 3) {
		if (c->recording)
			ret = cs42l63_set_gain(c);
	} else if (c->playing) {
		ret = cs42l63_set_volumes(c);
	}
	mutex_unlock(&c->lock);
	return ret ?: 1;
}

/* on/off controls: 0 the headphones (playback), 1 the headset's mic (capture) */
static bool *cs42l63_sw_field(struct cs42l63 *c, struct snd_kcontrol *kc)
{
	return kc->private_value ? &c->headset_mic : &c->hp;
}

static int cs42l63_sw_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	static const char * const srcs[] = { "Internal Mic", "Headset Mic" };

	if (kc->private_value)
		return snd_ctl_enum_info(ui, 1, ARRAY_SIZE(srcs), srcs);
	ui->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	ui->count = 1;
	ui->value.integer.min = 0;
	ui->value.integer.max = 1;
	return 0;
}

static int cs42l63_sw_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct cs42l63 *c = snd_soc_component_get_drvdata(snd_kcontrol_chip(kc));
	bool v;

	mutex_lock(&c->lock);
	v = *cs42l63_sw_field(c, kc);
	mutex_unlock(&c->lock);
	if (kc->private_value)
		uc->value.enumerated.item[0] = v;
	else
		uc->value.integer.value[0] = v;
	return 0;
}

/* moved while a stream runs: the old path off, the new one on, as iOS does */
static int cs42l63_sw_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct cs42l63 *c = snd_soc_component_get_drvdata(snd_kcontrol_chip(kc));
	bool v = kc->private_value ? uc->value.enumerated.item[0] : uc->value.integer.value[0];
	bool *f;
	int ret = 0;

	if (kc->private_value ? uc->value.enumerated.item[0] > 1 : uc->value.integer.value[0] > 1)
		return -EINVAL;
	mutex_lock(&c->lock);
	f = cs42l63_sw_field(c, kc);
	if (*f == v) {
		mutex_unlock(&c->lock);
		return 0;
	}
	if (!kc->private_value)
		ret = cs42l63_set_hp(c, v);
	else
		ret = cs42l63_set_source(c, v);
	mutex_unlock(&c->lock);
	return ret ?: 1;
}

/* the capture volume: 0..128 = -96..+32 dB, for either mic */
static const DECLARE_TLV_DB_SCALE(cs42l63_capture_tlv, -9600, 100, 0);

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
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Headphone Playback Volume",
		.info = cs42l63_vol_info,
		.get = cs42l63_vol_get,
		.put = cs42l63_vol_put,
		.private_value = 2,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Headphone Playback Switch",
		.info = cs42l63_sw_info,
		.get = cs42l63_sw_get,
		.put = cs42l63_sw_put,
		.private_value = 0,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Capture Volume",
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ,
		.tlv.p = cs42l63_capture_tlv,
		.info = cs42l63_vol_info,
		.get = cs42l63_vol_get,
		.put = cs42l63_vol_put,
		.private_value = 3,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Capture Source",
		.info = cs42l63_sw_info,
		.get = cs42l63_sw_get,
		.put = cs42l63_sw_put,
		.private_value = 1,
	},
};

/* --- the jack ----------------------------------------------------------------- */

static void cs42l63_notify(struct snd_soc_card *card, const char *name)
{
	struct snd_kcontrol *kc = snd_soc_card_get_kcontrol(card, name);

	if (kc)
		snd_ctl_notify(card->snd_card, SNDRV_CTL_EVENT_MASK_VALUE, &kc->id);
}

static bool cs42l63_plugged(struct cs42l63 *c, bool *in)
{
	int v = i2c_smbus_read_byte_data(c->i2c, CS42L63_JACK_REG);

	*in = v >= 0 && (v & CS42L63_JACK_IN);
	return v >= 0;
}

static void cs42l63_jack_work(struct work_struct *w)
{
	struct cs42l63 *c = container_of(to_delayed_work(w), struct cs42l63, jack_work);
	struct snd_soc_card *card = c->comp->card;
	bool in, again, hp_moved, src_moved;
	int status;

	if (!cs42l63_plugged(c, &in) || in == c->jack_in)
		goto out;
	/* the mic contact is measured next: let a moving plug settle first */
	msleep(CS42L63_JACK_SETTLE_MS);
	if (!cs42l63_plugged(c, &again) || again != in)
		goto out;

	mutex_lock(&c->lock);
	c->jack_in = in;
	/* the contacts and the mic's bias first, then the sound onto them */
	if (in)
		cs42l63_plug_in(c);
	else
		cs42l63_plug_out(c);
	hp_moved = c->hp != in;
	if (cs42l63_set_hp(c, in))
		dev_warn(c->dev, "could not move playback to the %s\n", in ? "headphones" : "speaker");
	/* capture follows a headset's mic in and out, as iOS routes it */
	src_moved = c->headset_mic != (in && c->mic);
	if (cs42l63_set_source(c, in && c->mic))
		dev_warn(c->dev, "could not move capture to the %s mic\n",
			 in && c->mic ? "headset's" : "built-in");
	status = !in ? 0 : c->mic ? SND_JACK_HEADSET : SND_JACK_HEADPHONE;
	mutex_unlock(&c->lock);

	snd_soc_jack_report(&c->jack, status, SND_JACK_HEADSET);
	if (hp_moved)
		cs42l63_notify(card, "Headphone Playback Switch");
	if (src_moved)
		cs42l63_notify(card, "Capture Source");
out:
	queue_delayed_work(system_power_efficient_wq, &c->jack_work,
			   msecs_to_jiffies(CS42L63_JACK_POLL_MS));
}

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

	mutex_lock(&c->lock);
	if (stream == SNDRV_PCM_STREAM_CAPTURE) {
		if (!mute && !c->recording) {
			ret = cs42l63_in_start(c);
			c->recording = !ret;
		} else if (mute && c->recording) {
			ret = RUN(c, input_stop);
			c->recording = false;
		}
	} else if (!mute && !c->playing) {
		/* the start sequence writes the volumes set (cs42l63_vol_subst) */
		ret = cs42l63_out_start(c);
		c->playing = !ret;
	} else if (mute && c->playing) {
		ret = cs42l63_out_stop(c);
		c->playing = false;
	}
	mutex_unlock(&c->lock);
	return ret;
}

static const struct snd_soc_dai_ops cs42l63_dai_ops = {
	.hw_params = cs42l63_hw_params,
	.mute_stream = cs42l63_mute_stream,
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
	.capture = {
		.stream_name = "Capture",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.symmetric_rate = 1,		/* one ASP clock for both */
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

	c->comp = comp;
	/* one jack, its two halves as "Headphone Jack" and "Headset Mic Jack" */
	ret = snd_soc_card_jack_new_pins(comp->card, "Headset", SND_JACK_HEADSET, &c->jack, NULL, 0);
	ret = ret ?: snd_jack_add_new_kctl(c->jack.jack, "Headphone", SND_JACK_HEADPHONE);
	ret = ret ?: snd_jack_add_new_kctl(c->jack.jack, "Headset Mic", SND_JACK_MICROPHONE);
	if (ret)
		dev_warn(c->dev, "no jack device (%d); the jack is still followed\n", ret);
	INIT_DELAYED_WORK(&c->jack_work, cs42l63_jack_work);
	queue_delayed_work(system_power_efficient_wq, &c->jack_work, 0);
	return 0;
}

static void cs42l63_component_remove(struct snd_soc_component *comp)
{
	struct cs42l63 *c = snd_soc_component_get_drvdata(comp);

	cancel_delayed_work_sync(&c->jack_work);
	mutex_lock(&c->lock);
	if (c->playing)
		cs42l63_out_stop(c);
	c->playing = false;
	if (c->recording)
		RUN(c, input_stop);
	c->recording = false;
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

static void cs42l63_put_hsw(void *data)
{
	put_device(&((struct i2c_client *)data)->dev);
}

static int cs42l63_i2c_probe(struct i2c_client *i2c)
{
	struct device *dev = &i2c->dev;
	struct device_node *np;
	struct cs42l63 *c;
	int ret;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->i2c = i2c;
	c->dev = dev;
	mutex_init(&c->lock);
	i2c_set_clientdata(i2c, c);
	/* iOS's defaults: the levels at the top -- loud.  The master a little
	 * lower, and the headphones lower still: ears are close */
	c->speaker = CS42L63_VOL_MAX;
	c->headphone = 50;
	c->master = 48;
	/* either mic at +24 dB, 8 dB under iOS's default (a voice close to a
	 * headset's mic comes near full scale there); 0, -96 dB, is silence */
	c->capture = 120;

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
	np = of_parse_phandle(dev->of_node, "headset-switch", 0);
	if (np) {
		c->hsw = of_find_i2c_device_by_node(np);
		of_node_put(np);
		if (!c->hsw)
			return -EPROBE_DEFER;
		ret = devm_add_action_or_reset(dev, cs42l63_put_hsw, c->hsw);
		if (ret)
			return ret;
	}

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
