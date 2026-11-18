// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cirrus Logic CS42L63, Apple's codec of the A5 and A5X iPads: its serial
 * port (ASP) takes 16-bit I2S from the SoC's I2S0, and line out 2 drives the
 * speaker through an external amplifier with its own enable GPIO.  The iPad 3
 * (J1) uses that much; the iPad 2 (K93) also its headphone output, its two
 * microphone inputs -- the built-in one on digital mic input B, a headset's
 * -- and the headset jack.
 *
 * The CS42L63 is Apple's part; no datasheet is public.  It is a sibling of
 * the CS42L73 (device ID 42 a6 3x, the same rate codes and volume coding) with
 * a flat 8-bit register map of its own.  This driver writes what iOS's
 * AppleCS42L63Audio writes: its sequences were recorded by running that
 * driver in an emulator, iOS 9.3.5's for the J1 (docs/research/j1-a5x.md)
 * and iOS 8.4.1's for the K93 (docs/research/k93-audio.md,
 * tools/audio/k93-cs42l63-sequences.py), and they agree wherever both boards
 * use them.  They are compiled in from cs42l63-seq.h, which
 * tools/audio/k93-seq2c.py generates from the recordings.  What most
 * registers mean is not known, so they are named by number.
 *
 * What a board has comes from its device tree:
 *
 *   clocks "mclk"        the codec's 6 MHz MCLK, from the SoC's I2S0 (needed
 *                        before it powers up; until it is there, probing
 *                        is deferred)
 *   speaker-amp-gpios    the speaker amplifier's enable: on after the path is
 *                        up, off before it goes down, as iOS orders them
 *   reset-gpios          the codec's reset (K93: PMU GPIO 13, active low),
 *                        pulsed at the cold power-up as iOS does; without it,
 *                        as iBoot leaves it
 *   headset-switch       the TS3A8235 between the jack and the codec (K93):
 *                        the headphones, a headset's microphone and the jack,
 *                        watched by polling the codec's Mikey block
 *   apple,internal-mic   the built-in microphone on digital mic input B (K93)
 *
 * Power.  With nothing to watch, the codec is powered for streams only:
 * "Codec Power", a DAPM supply, powers it up before a stream and down once
 * the pmdown time has passed, and MCLK runs while it is up.  A board with a
 * headset jack keeps it powered from the start -- the Mikey block that sees a
 * plug go in is the codec's -- as iOS does.
 *
 * Outputs.  A stream's start (unmute) runs iOS's speaker_start or hp_start,
 * its end (mute) speaker_stop or hp_stop: the path, the levels, and the
 * amplifier's enable last on and first off.  iOS's start sequences open an
 * output at its maximum; every such write here carries the volume set
 * instead, so nothing is ever louder than asked, not even for a millisecond.
 *
 * Volumes, in iOS's units:
 *   "Speaker Playback Volume"    line out 2's level, 0x21/0x22 = 0x41 + v,
 *                                0..62 = -76..-1 dB (2 dB steps to -50, then
 *                                1 dB); with no headset jack it is the board's
 *                                volume and starts at -22 dB, with one it
 *                                starts at iOS's level (62) and the master is
 *                                the volume
 *   "Headphone Playback Volume"  the headphones' level, 0x23/0x25, the same
 *                                coding (headset jack)
 *   "Master Playback Volume"     the mixer's attenuation of the output in use
 *                                (speaker 0x45/0x49, headphones 0x3d/0x41 =
 *                                62 - v), 1 dB a step (headset jack)
 * and "Headphone Playback Switch" sends playback to the headphones instead of
 * the speaker, at once if a stream plays (the jack sets it).
 *
 * Capture runs iOS's input_internal_mic or input_headset_mic ("Capture
 * Source"), input_stop after, and "Capture Volume" is iOS's input volume
 * (control 0x16f) through iOS's own mapping for that source (cs42l63_gain_*:
 * a boost, the analog stage, the digital volume), on one dB scale for both:
 * 0-128 = -96 to +32 dB in 1 dB steps.  iOS records a mic -- one, on the
 * ASP's left channel -- in mono; here it is also mixed into the right one
 * (0x58 or 0x5b), so a stereo recording has it twice.
 *
 * The jack: the Mikey block's register 0x77 bit 7 is set while a plug is in
 * (0x7b latches bit 6 on insertion, bit 5 on removal, and clears when read),
 * measured on the K93.  Its interrupt line (SoC GPIO 15) stays masked as
 * iOS's init leaves it, so the jack is polled, twice a second.  When a plug
 * goes in, this decides what iOS's AppleCSCodecMikey decides for a headset
 * without Apple's remote (docs/research/k93-audio.md):
 *   - it measures the plug's mic contact in the CTIA wiring of the TS3A8235
 *     (the jack's ground and mic contacts, 0x42), and in the OMTP one (0x43)
 *     only if CTIA reads reverse-bias or short -- iOS measures both but
 *     decides only then: the mic bias on (0x74 = 0x06), the comparators'
 *     reference at 0x17 and 0x36 (0x75), the answer in 0x78 -- open, mic,
 *     reverse-bias or short;
 *   - keeps the wiring iOS would (OMTP only if CTIA reads reverse-bias or
 *     short and OMTP does not), the bias off again.
 * Then playback moves to the headphones and, with a mic, capture to it; both
 * move back when the plug comes out (the bias off, the switch at 0x00: as
 * iBoot leaves it, it passes nothing until it is written).  "Headphone Jack"
 * and "Headset Mic Jack" tell userspace, the moved controls notified.
 *
 * The headset's mic is biased while it records, as iOS's MicOpen state does
 * it (0x75 = 0x3c, 0x74 = 0x1e) -- without the bias it records exact zeros.
 * iOS keeps it biased as long as the plug is in, to hear the headset's
 * button; here it is off otherwise, because a plug pulled out with the bias
 * on clicks in the earbuds.  The bias comes on behind iOS's pop guard (the
 * switch in its guard state 0x06 meanwhile) and goes off bare, as in iOS:
 * heard with earbuds in, both are silent that way, and click the other way
 * round.  Apple's remote (Mikey's own protocol) and a headset's button are
 * not done.
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

#define CS42L63_DEVID_AB	0x01	/* 0x42 0xa6 0x3x */
#define CS42L63_REV_ID		0x05
#define CS42L63_PWR1		0x06	/* bit 0: power down */
#define CS42L63_SPC		0x11	/* bits 3:0: the sample rate code */
#define CS42L63_IS1		0x62	/* interrupt status, read to clear */
#define CS42L63_CTL_KEY		0x7d

#define CS42L63_VOL_MAX		62	/* 0x7f = -1 dB, iOS's maximum */
#define CS42L63_VOL_SPEAKER	41	/* -22 dB: a speaker-only board's start */
#define CS42L63_GAIN_MAX	(ARRAY_SIZE(cs42l63_gain_internal) - 1)
static_assert(ARRAY_SIZE(cs42l63_gain_internal) == ARRAY_SIZE(cs42l63_gain_headset));

struct cs42l63 {
	struct i2c_client *i2c;
	struct device *dev;
	struct gpio_desc *reset;	/* active low: 1 = held in reset; optional */
	struct gpio_desc *spk;		/* the amplifier's enable: 1 = plays; optional */
	struct clk *mclk;
	bool mclk_on;
	struct mutex lock;
	bool keep_powered;		/* a headset jack to watch: powered from the start */
	bool imic;			/* the built-in microphone (apple,internal-mic) */
	bool powered;			/* out of power-down, every block configured */
	bool cold;			/* not powered up since the reset (reset-gpios) */
	unsigned int rate_code;		/* 0x11's, kept for the next power-up */
	bool playing, recording;
	bool hp;			/* playback to the headphones, not the speaker */
	bool headset_mic;		/* capture from the headset's mic, not the built-in one */
	unsigned int speaker[2], headphone[2];	/* 0..62, iOS's units, A and B */
	unsigned int master;		/* 0..62; 62 (no attenuation) without a jack */
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

/* the mixer's attenuation of the output in use: 0 dB without a master */
static u8 cs42l63_att(struct cs42l63 *c)
{
	return CS42L63_VOL_MAX - c->master;
}

/*
 * The value to write instead of what a sequence wants: an output's analog
 * level or the active mixer entry, unless the sequence mutes it (0x40 for a
 * level, 0x3f / 0x7f for a mixer entry), is the volume that is set.
 */
static u8 cs42l63_vol_subst(struct cs42l63 *c, u8 reg, u8 val)
{
	u8 att = cs42l63_att(c);

	switch (reg) {
	case 0x21: case 0x22:			/* speaker (line out 2) level A / B */
		return val == 0x40 ? val : 0x41 + c->speaker[reg - 0x21];
	case 0x23: case 0x25:			/* headphone level A / B */
		return val == 0x40 ? val : 0x41 + c->headphone[reg == 0x25];
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

static int cs42l63_upd(struct cs42l63 *c, u8 reg, u8 mask, u8 val)
{
	int v = i2c_smbus_read_byte_data(c->i2c, reg);

	return v < 0 ? v : i2c_smbus_write_byte_data(c->i2c, reg, (v & ~mask) | val);
}

/* --- power ---------------------------------------------------------------- */

/*
 * iOS's power-up, MCLK running: on a cold start the reset pulsed (where the
 * board has the line), the control key set for 10 ms and released, every
 * block configured and out of power-down, the rate, every output off and
 * muted, the inputs configured and off (a board that records).
 */
static int cs42l63_power_up(struct cs42l63 *c)
{
	int v, ret;

	if (c->reset && c->cold) {
		gpiod_set_value_cansleep(c->reset, 1);
		msleep(10);
		gpiod_set_value_cansleep(c->reset, 0);
		usleep_range(1000, 2000);
		c->cold = false;
	}
	ret = i2c_smbus_write_byte_data(c->i2c, CS42L63_CTL_KEY, 0xa5);
	if (ret)
		return ret;
	usleep_range(10000, 11000);
	ret = i2c_smbus_write_byte_data(c->i2c, CS42L63_CTL_KEY, 0x00);
	ret = ret ?: RUN(c, power_up_seq);
	ret = ret ?: cs42l63_upd(c, CS42L63_SPC, 0x0f, c->rate_code);
	gpiod_set_value_cansleep(c->spk, 0);
	ret = ret ?: RUN(c, outputs_idle);
	if (!ret && (c->imic || c->hsw))
		ret = RUN(c, inputs_idle);
	if (ret)
		return ret;
	/* bit 7 latches once the codec runs from MCLK */
	v = i2c_smbus_read_byte_data(c->i2c, CS42L63_IS1);
	dev_dbg(c->dev, "powered up, status %02x\n", v);
	c->powered = true;
	return 0;
}

/*
 * iOS's power-down: the outputs muted and off, the headset block's own part
 * (a board with the jack), into power-down -- iOS waits for 0x62 bit 7, 10 ms
 * a try, and goes on without it -- and the control key set again.
 */
static void cs42l63_power_down(struct cs42l63 *c)
{
	int i, v;

	c->powered = false;
	RUN(c, power_down_outputs);
	if (c->hsw)
		RUN(c, mikey_off);
	cs42l63_upd(c, MIKEY_REF, 0x80, 0x00);
	cs42l63_upd(c, CS42L63_PWR1, 0x01, 0x01);
	for (i = 0; i < 10; i++) {
		v = i2c_smbus_read_byte_data(c->i2c, CS42L63_IS1);
		if (v < 0 || (v & 0x80))
			break;
		usleep_range(10000, 11000);
	}
	cs42l63_upd(c, CS42L63_PWR1, 0x40, 0x40);
	usleep_range(10000, 11000);
	i2c_smbus_write_byte_data(c->i2c, CS42L63_CTL_KEY, 0xa5);
}

/* MCLK, then the power-up; undone if that fails */
static int cs42l63_power_on(struct cs42l63 *c)
{
	int ret;

	if (!c->mclk_on) {
		ret = clk_prepare_enable(c->mclk);
		if (ret) {
			dev_err(c->dev, "no MCLK: %d\n", ret);
			return ret;
		}
		c->mclk_on = true;
	}
	ret = cs42l63_power_up(c);
	if (ret) {
		dev_err(c->dev, "power-up failed: %d\n", ret);
		clk_disable_unprepare(c->mclk);
		c->mclk_on = false;
	}
	return ret;
}

static void cs42l63_power_off(struct cs42l63 *c)
{
	if (c->powered)
		cs42l63_power_down(c);
	if (c->mclk_on)
		clk_disable_unprepare(c->mclk);
	c->mclk_on = false;
}

/* "Codec Power": for streams only, where nothing keeps the codec up */
static int cs42l63_power_event(struct snd_soc_dapm_widget *w,
			       struct snd_kcontrol *kcontrol, int event)
{
	struct cs42l63 *c = snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));
	int ret = 0;

	if (c->keep_powered)
		return 0;
	mutex_lock(&c->lock);
	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		ret = cs42l63_power_on(c);
		break;
	case SND_SOC_DAPM_POST_PMD:
		/* DAPM goes on after a failed PRE_PMU: undo only what was done */
		cs42l63_power_off(c);
		break;
	}
	mutex_unlock(&c->lock);
	return ret;
}

/* --- outputs -------------------------------------------------------------- */

/* the volumes of the output in use, as iOS writes them while it plays */
static int cs42l63_set_volumes(struct cs42l63 *c)
{
	unsigned int att = cs42l63_att(c);
	const unsigned int *lvl = c->hp ? c->headphone : c->speaker;
	u8 m0 = c->hp ? 0x3d : 0x45, m1 = c->hp ? 0x41 : 0x49;
	u8 l0 = c->hp ? 0x23 : 0x21, l1 = c->hp ? 0x25 : 0x22;
	int ret;

	ret = i2c_smbus_write_byte_data(c->i2c, m0, att);
	ret = ret ?: i2c_smbus_write_byte_data(c->i2c, m1, 0x40 | att);
	ret = ret ?: i2c_smbus_write_byte_data(c->i2c, l0, 0x41 + lvl[0]);
	ret = ret ?: i2c_smbus_write_byte_data(c->i2c, l1, 0x41 + lvl[1]);
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

/* --- the Mikey block and the headset switch ----------------------------------- */

static int cs42l63_hsw_set(struct cs42l63 *c, u8 state)
{
	c->hsw_state = state;
	return c->hsw ? i2c_smbus_write_byte_data(c->hsw, TS3A8235_STATE, state) : 0;
}

/*
 * One of iOS's Mikey modes as the K93 runs them (AppleCSCodecMikey 0x8091c464,
 * modes 0/3, 4 and 8): 0x70 = 0x01, 0x71 bits 7:6 set, 0x74 = the mode, then
 * its settling time.  The bias coming on (0x74 bits 2:1 other than 01) while a
 * plug is connected goes behind iOS's pop guard (0x8091c63c,
 * headphone-pop-workaround = 2): the switch in its guard state for the
 * slow-ramp delay while 0x74 changes.  Heard on the iPad with earbuds in, as
 * iOS has it: the bias coming on clicks bare and behind a 0x46 guard, not
 * behind 0x06; going off it is silent bare, and clicks behind 0x06.
 */
static int cs42l63_mikey_mode(struct cs42l63 *c, u8 mode)
{
	bool on = (mode & 6) != 2;
	u8 state = c->hsw_state;
	int ret;

	ret = i2c_smbus_write_byte_data(c->i2c, MIKEY_CTL, 0x01);
	ret = ret ?: cs42l63_upd(c, MIKEY_CTL2, 0xc0, 0xc0);
	if (!ret && on && !c->bias && c->hsw && state != TS3A8235_NONE) {
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

/* the bias off: reference 0x36 and mode 0, as iOS leaves headphones */
static int cs42l63_bias_off(struct cs42l63 *c)
{
	int ret = cs42l63_mikey_mode(c, MIKEY_MODE_OFF);

	return cs42l63_upd(c, MIKEY_REF, 0x3f, 0x36) ?: ret;
}

/* the bias on for the headset's mic: iOS's MicOpen, reference 0x3c and mode 8 */
static int cs42l63_bias_mic(struct cs42l63 *c)
{
	int ret = cs42l63_upd(c, MIKEY_REF, 0x3f, 0x3c);

	return ret ?: cs42l63_mikey_mode(c, MIKEY_MODE_MIC);
}

/*
 * What the mic contact reads in a wiring of the switch (iOS's 0x8091bc00 and
 * 0x8091bd14): the bias on, reference 0x17 -- shorted, or reversed (above
 * it) -- then reference 0x36: a mic above it, else open.  The bias is left
 * on.  iOS measures in the switch's slow states, the bias coming on bare;
 * here in the normal ones, behind the guard: the same reading, no click.
 */
static int cs42l63_measure(struct cs42l63 *c, u8 wiring)
{
	int v, ret;

	ret = cs42l63_hsw_set(c, wiring);
	ret = ret ?: cs42l63_upd(c, MIKEY_REF, 0x3f, 0x17);
	ret = ret ?: cs42l63_mikey_mode(c, MIKEY_MODE_MEASURE);
	if (ret)
		return ret;
	msleep(TS3A8235_RAMP_MS);
	v = i2c_smbus_read_byte_data(c->i2c, MIKEY_SENSE);
	if (v < 0)
		return v;
	if (v & 1)
		return LINE_SHORT;
	if (v & 2)
		return LINE_REVERSE;
	ret = cs42l63_upd(c, MIKEY_REF, 0x3f, 0x36);
	usleep_range(1000, 2000);
	v = ret ?: i2c_smbus_read_byte_data(c->i2c, MIKEY_SENSE);
	if (v < 0)
		return v;
	return (v & 2) ? LINE_MIC : LINE_OPEN;
}

/*
 * A plug went in: its wiring and its mic, decided as iOS's detection decides
 * them.  iOS measures both wirings, but keeps CTIA unless CTIA reads
 * reverse-bias or short and OMTP does not -- so OMTP is measured here only
 * when CTIA reads one of those.  The bias goes off again after: here it is
 * on only while the headset's mic records (cs42l63_in_start), not all the
 * time as in iOS, which watches the headset's button with it -- a plug
 * pulled out with the bias on clicks in the earbuds (heard on the iPad).
 */
static void cs42l63_plug_in(struct cs42l63 *c)
{
	int r[2] = { LINE_OPEN, LINE_OPEN }, ret;
	bool omtp = false;

	c->mic = false;
	ret = cs42l63_measure(c, TS3A8235_CTIA);
	if (ret >= 0) {
		r[0] = ret;
		ret = 0;
	}
	if (!ret && r[0] >= LINE_REVERSE) {
		ret = cs42l63_bias_off(c);
		ret = ret ?: cs42l63_measure(c, TS3A8235_OMTP);
		if (ret >= 0) {
			r[1] = ret;
			ret = 0;
			omtp = r[1] <= LINE_MIC;
		}
	}
	c->mic = !ret && r[omtp] == LINE_MIC;
	ret = cs42l63_bias_off(c) ?: ret;
	ret = cs42l63_hsw_set(c, omtp ? TS3A8235_OMTP : TS3A8235_CTIA) ?: ret;
	if (ret)
		dev_warn(c->dev, "could not measure the plug: %d\n", ret);
	dev_info(c->dev, "plug in: the mic contact reads %s (CTIA), %s (OMTP): %s wiring, %s\n",
		 cs42l63_line[r[0]], r[0] >= LINE_REVERSE ? cs42l63_line[r[1]] : "not measured",
		 omtp ? "OMTP" : "CTIA", c->mic ? "a headset with a mic" : "headphones");
}

/* the plug came out: the switch open first, then the bias off */
static void cs42l63_plug_out(struct cs42l63 *c)
{
	int ret;

	ret = cs42l63_hsw_set(c, TS3A8235_NONE);
	ret = cs42l63_bias_off(c) ?: ret;
	if (ret)
		dev_warn(c->dev, "could not release the jack: %d\n", ret);
	c->mic = false;
	dev_info(c->dev, "plug out\n");
}

/* --- capture ------------------------------------------------------------------ */

/* the input path on: a headset's mic biased first, behind the guard */
static int cs42l63_in_start(struct cs42l63 *c)
{
	int ret;

	if (c->headset_mic) {
		ret = c->mic ? cs42l63_bias_mic(c) : 0;
		ret = ret ?: RUN(c, input_headset_mic);
	} else {
		ret = RUN(c, input_internal_mic);
	}
	return ret ?: cs42l63_set_gain(c);
}

/* the input path off, and a headset's mic bias with it */
static int cs42l63_in_stop(struct cs42l63 *c)
{
	int ret = RUN(c, input_stop);

	if (c->bias && c->jack_in)
		ret = cs42l63_bias_off(c) ?: ret;
	return ret;
}

/* capture from the headset's mic or the built-in one, moved at once while recording */
static int cs42l63_set_source(struct cs42l63 *c, bool headset)
{
	int ret = 0;

	if (c->headset_mic == headset)
		return 0;
	if (c->recording) {
		ret = cs42l63_in_stop(c);
		c->headset_mic = headset;
		ret = ret ?: cs42l63_in_start(c);
	} else {
		c->headset_mic = headset;
	}
	return ret;
}

/* --- controls ------------------------------------------------------------- */

enum { CTL_SPEAKER, CTL_HEADPHONE, CTL_MASTER, CTL_CAPTURE };

static unsigned int cs42l63_vol_max(struct snd_kcontrol *kc)
{
	return kc->private_value == CTL_CAPTURE ? CS42L63_GAIN_MAX : CS42L63_VOL_MAX;
}

/* A and B for the outputs' levels, one value for the master and the input */
static unsigned int cs42l63_vol_count(struct snd_kcontrol *kc)
{
	return kc->private_value <= CTL_HEADPHONE ? 2 : 1;
}

static int cs42l63_vol_info(struct snd_kcontrol *kc, struct snd_ctl_elem_info *ui)
{
	ui->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	ui->count = cs42l63_vol_count(kc);
	ui->value.integer.min = 0;
	ui->value.integer.max = cs42l63_vol_max(kc);
	return 0;
}

static unsigned int *cs42l63_vol_field(struct cs42l63 *c, struct snd_kcontrol *kc)
{
	switch (kc->private_value) {
	case CTL_SPEAKER:
		return c->speaker;
	case CTL_HEADPHONE:
		return c->headphone;
	case CTL_CAPTURE:
		return &c->capture;
	}
	return &c->master;
}

static int cs42l63_vol_get(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct cs42l63 *c = snd_soc_component_get_drvdata(snd_kcontrol_chip(kc));
	unsigned int *f, i;

	mutex_lock(&c->lock);
	f = cs42l63_vol_field(c, kc);
	for (i = 0; i < cs42l63_vol_count(kc); i++)
		uc->value.integer.value[i] = f[i];
	mutex_unlock(&c->lock);
	return 0;
}

static int cs42l63_vol_put(struct snd_kcontrol *kc, struct snd_ctl_elem_value *uc)
{
	struct cs42l63 *c = snd_soc_component_get_drvdata(snd_kcontrol_chip(kc));
	unsigned int *f, i, n = cs42l63_vol_count(kc);
	bool changed = false;
	int ret = 0;

	for (i = 0; i < n; i++)
		if (uc->value.integer.value[i] < 0 || uc->value.integer.value[i] > cs42l63_vol_max(kc))
			return -EINVAL;
	mutex_lock(&c->lock);
	f = cs42l63_vol_field(c, kc);
	for (i = 0; i < n; i++) {
		changed |= f[i] != uc->value.integer.value[i];
		f[i] = uc->value.integer.value[i];
	}
	if (changed) {
		if (kc->private_value == CTL_CAPTURE) {
			if (c->recording)
				ret = cs42l63_set_gain(c);
		} else if (c->playing) {
			ret = cs42l63_set_volumes(c);
		}
	}
	mutex_unlock(&c->lock);
	return ret ?: changed;
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

/* -76 dB in 2 dB steps to -50, then 1 dB steps (the CS42L73's coding) */
static const DECLARE_TLV_DB_RANGE(cs42l63_vol_tlv,
	0, 13, TLV_DB_SCALE_ITEM(-7600, 200, 0),
	14, CS42L63_VOL_MAX, TLV_DB_SCALE_ITEM(-4900, 100, 0)
);
/* the mixer's attenuation, 1 dB a step down from 0 dB */
static const DECLARE_TLV_DB_SCALE(cs42l63_master_tlv, -6200, 100, 0);
/* the input volume: 0..128 = -96..+32 dB, for either mic */
static const DECLARE_TLV_DB_SCALE(cs42l63_capture_tlv, -9600, 100, 0);

#define CS42L63_VOLUME(xname, xtlv, which) {					\
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname),			\
	.access = SNDRV_CTL_ELEM_ACCESS_READWRITE | SNDRV_CTL_ELEM_ACCESS_TLV_READ, \
	.tlv.p = (xtlv), .info = cs42l63_vol_info, .get = cs42l63_vol_get,	\
	.put = cs42l63_vol_put, .private_value = (which) }
#define CS42L63_SWITCH(xname, which) {						\
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname),			\
	.info = cs42l63_sw_info, .get = cs42l63_sw_get, .put = cs42l63_sw_put,	\
	.private_value = (which) }

/* every board: its speaker */
static const struct snd_kcontrol_new cs42l63_speaker_controls[] = {
	CS42L63_VOLUME("Speaker Playback Volume", cs42l63_vol_tlv, CTL_SPEAKER),
};

/* a board with the headset jack: the headphones, the master over both */
static const struct snd_kcontrol_new cs42l63_headphone_controls[] = {
	CS42L63_VOLUME("Master Playback Volume", cs42l63_master_tlv, CTL_MASTER),
	CS42L63_VOLUME("Headphone Playback Volume", cs42l63_vol_tlv, CTL_HEADPHONE),
	CS42L63_SWITCH("Headphone Playback Switch", 0),
};

/* a board that records */
static const struct snd_kcontrol_new cs42l63_capture_controls[] = {
	CS42L63_VOLUME("Capture Volume", cs42l63_capture_tlv, CTL_CAPTURE),
};

/* the built-in mic and a headset's: which one records */
static const struct snd_kcontrol_new cs42l63_source_controls[] = {
	CS42L63_SWITCH("Capture Source", 1),
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

/* kept for the next power-up, and written now if the codec is up */
static int cs42l63_hw_params(struct snd_pcm_substream *ss, struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct cs42l63 *c = snd_soc_component_get_drvdata(dai->component);
	int code = cs42l63_rate_code(params_rate(params)), ret = 0;

	if (code < 0)
		return code;
	mutex_lock(&c->lock);
	c->rate_code = code;
	if (c->powered)
		ret = cs42l63_upd(c, CS42L63_SPC, 0x0f, code);
	mutex_unlock(&c->lock);
	return ret;
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

/* a stream's start and end: its output's or its input's path on and off */
static int cs42l63_mute_stream(struct snd_soc_dai *dai, int mute, int stream)
{
	struct cs42l63 *c = snd_soc_component_get_drvdata(dai->component);
	int ret = 0;

	mutex_lock(&c->lock);
	if (!c->powered) {
		/* the power-up failed (Codec Power): nothing to switch */
		ret = mute ? 0 : -EIO;
	} else if (stream == SNDRV_PCM_STREAM_CAPTURE) {
		if (!mute && !c->recording) {
			ret = cs42l63_in_start(c);
			c->recording = !ret;
		} else if (mute && c->recording) {
			ret = cs42l63_in_stop(c);
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
	.set_fmt = cs42l63_set_fmt,
	.mute_stream = cs42l63_mute_stream,
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
	.capture = {
		.stream_name = "ASP Capture",
		.channels_min = 2,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |
			 SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.symmetric_rate = 1,		/* one ASP clock for both */
	.ops = &cs42l63_dai_ops,
};

/* --- component -------------------------------------------------------------- */

static const struct snd_soc_dapm_widget cs42l63_widgets[] = {
	SND_SOC_DAPM_SUPPLY("Codec Power", SND_SOC_NOPM, 0, 0, cs42l63_power_event,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_AIF_IN("ASP RX", "ASP Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("ASP TX", "ASP Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_OUTPUT("OUT"),
	SND_SOC_DAPM_INPUT("MIC"),
};

/* the paths are switched by the streams themselves (mute_stream); DAPM
 * only powers the codec around them */
static const struct snd_soc_dapm_route cs42l63_routes[] = {
	{ "ASP RX", NULL, "Codec Power" },
	{ "ASP TX", NULL, "Codec Power" },
	{ "OUT", NULL, "ASP RX" },
	{ "ASP TX", NULL, "MIC" },
};

static int cs42l63_add_controls(struct snd_soc_component *comp)
{
	struct cs42l63 *c = snd_soc_component_get_drvdata(comp);
	int ret;

	ret = snd_soc_add_component_controls(comp, cs42l63_speaker_controls,
					     ARRAY_SIZE(cs42l63_speaker_controls));
	if (!ret && c->hsw)
		ret = snd_soc_add_component_controls(comp, cs42l63_headphone_controls,
						     ARRAY_SIZE(cs42l63_headphone_controls));
	if (!ret && (c->imic || c->hsw))
		ret = snd_soc_add_component_controls(comp, cs42l63_capture_controls,
						     ARRAY_SIZE(cs42l63_capture_controls));
	if (!ret && c->imic && c->hsw)
		ret = snd_soc_add_component_controls(comp, cs42l63_source_controls,
						     ARRAY_SIZE(cs42l63_source_controls));
	return ret;
}

/* the ID can only be read out of reset and with MCLK: a board with the reset
 * line reads it after the first power-up, one without at probe */
static int cs42l63_check_id(struct cs42l63 *c)
{
	int id[3], rev, i;

	for (i = 0; i < 3; i++) {
		id[i] = i2c_smbus_read_byte_data(c->i2c, CS42L63_DEVID_AB + i);
		if (id[i] < 0)
			return dev_err_probe(c->dev, id[i], "no answer\n");
	}
	if (id[0] != 0x42 || id[1] != 0xa6 || (id[2] & 0xf0) != 0x30)
		return dev_err_probe(c->dev, -ENODEV, "device ID %02x%02x%02x, not a CS42L63\n",
				     id[0], id[1], id[2]);
	rev = i2c_smbus_read_byte_data(c->i2c, CS42L63_REV_ID);
	dev_info(c->dev, "CS42L63, device ID %02x %02x %02x, revision %02x\n",
		 id[0], id[1], id[2], rev & 0xff);
	return 0;
}

static int cs42l63_component_probe(struct snd_soc_component *comp)
{
	struct cs42l63 *c = snd_soc_component_get_drvdata(comp);
	int ret;

	c->comp = comp;
	ret = cs42l63_add_controls(comp);
	if (ret)
		return ret;
	if (!c->keep_powered)
		return 0;

	/* the jack: powered from now on, as iOS's cold init leaves it */
	mutex_lock(&c->lock);
	ret = cs42l63_power_on(c);
	mutex_unlock(&c->lock);
	if (ret)
		return ret;
	if (c->reset && cs42l63_check_id(c))
		dev_warn(c->dev, "going on all the same, as iOS does\n");

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

	if (c->keep_powered)
		cancel_delayed_work_sync(&c->jack_work);
	mutex_lock(&c->lock);
	if (c->powered && c->playing)
		cs42l63_out_stop(c);
	c->playing = false;
	if (c->powered && c->recording)
		cs42l63_in_stop(c);
	c->recording = false;
	cs42l63_power_off(c);
	gpiod_set_value_cansleep(c->reset, 1);
	mutex_unlock(&c->lock);
}

static const struct snd_soc_component_driver cs42l63_component = {
	.probe = cs42l63_component_probe,
	.remove = cs42l63_component_remove,
	.dapm_widgets = cs42l63_widgets,
	.num_dapm_widgets = ARRAY_SIZE(cs42l63_widgets),
	.dapm_routes = cs42l63_routes,
	.num_dapm_routes = ARRAY_SIZE(cs42l63_routes),
	.use_pmdown_time = 1,
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
	c->rate_code = 9;			/* 48 kHz */
	c->cold = true;
	/* either mic at +24 dB, 8 dB under iOS's default (a voice close to a
	 * headset's mic comes near full scale there); 0, -96 dB, is silence */
	c->capture = 120;

	/*
	 * MCLK comes from the SoC's audio node, which registers it before it
	 * creates the sound card: until then, this probe is deferred.
	 */
	c->mclk = devm_clk_get(dev, "mclk");
	if (IS_ERR(c->mclk))
		return dev_err_probe(dev, PTR_ERR(c->mclk), "no MCLK\n");
	/* held in reset until the cold power-up lets it go, as iOS's start() does */
	c->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(c->reset))
		return dev_err_probe(dev, PTR_ERR(c->reset), "reset GPIO\n");
	/* the speaker amplifier, off until a stream needs it */
	c->spk = devm_gpiod_get_optional(dev, "speaker-amp", GPIOD_OUT_LOW);
	if (IS_ERR(c->spk))
		return dev_err_probe(dev, PTR_ERR(c->spk), "speaker amplifier GPIO\n");
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
	c->imic = of_property_read_bool(dev->of_node, "apple,internal-mic");
	c->keep_powered = c->hsw;

	if (c->hsw) {
		/* the master is the volume, over iOS's own levels: the speaker's
		 * top, the headphones lower -- ears are close */
		c->speaker[0] = c->speaker[1] = CS42L63_VOL_MAX;
		c->headphone[0] = c->headphone[1] = 50;
		c->master = 48;
	} else {
		/* the speaker's level is the volume */
		c->speaker[0] = c->speaker[1] = CS42L63_VOL_SPEAKER;
		c->master = CS42L63_VOL_MAX;
	}

	if (!c->reset) {
		ret = cs42l63_check_id(c);
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

static struct i2c_driver cs42l63_i2c_driver = {
	.driver = {
		.name = "cs42l63",
		.of_match_table = cs42l63_of_match,
	},
	.id_table = cs42l63_id,
	.probe = cs42l63_i2c_probe,
};
module_i2c_driver(cs42l63_i2c_driver);

MODULE_DESCRIPTION("Cirrus Logic CS42L63 codec (iPad 2, iPad 3), iOS's sequences");
MODULE_LICENSE("GPL");
