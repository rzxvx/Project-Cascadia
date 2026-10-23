// SPDX-License-Identifier: GPL-2.0-only
/*
 * p105-audio-dt -- add the speakers' device-tree nodes to a running kernel.
 *
 * Research tool for docs/research/p105-audio.md, not part of the image: it
 * lets the audio drivers be developed as modules on a kernel whose DTB
 * predates them.  The nodes are the ones dts/p105ap.dts carries; they are
 * added with an of_changeset, so the platform and I2C devices appear the
 * way they would at boot and the drivers' DT paths are the ones exercised.
 *
 *   insmod p105-audio-dt.ko [dma_channel=0x17]
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/string.h>

static unsigned int dma_channel = 0x17;
module_param(dma_channel, uint, 0444);
MODULE_PARM_DESC(dma_channel, "CDMA channel for MCA0 TX (iOS: 0x17)");

static struct of_changeset ocs;

/*
 * A node attached by a changeset takes its name and phandle from properties
 * the changeset only adds after attaching it; an OF_OVERLAY node keeps the
 * ones set here.
 */
static struct device_node *node(struct device_node *parent, const char *full, u32 ph)
{
	struct device_node *np = of_changeset_create_node(&ocs, parent, full);

	if (!np)
		return NULL;
	np->name = kstrndup(full, strcspn(full, "@"), GFP_KERNEL);
	np->phandle = ph;
	of_node_set_flag(np, OF_OVERLAY);
	return np;
}

#define PH_MCA_PS	0x7ff0
#define PH_I2S1_PS	0x7ff1
#define PH_AUDIO	0x7ff2
#define PH_SPK_L	0x7ff3
#define PH_SPK_R	0x7ff4

static struct device_node *gate(struct device_node *soc, const char *name,
				u32 reg, u32 ph, u32 refclk)
{
	struct device_node *np = node(soc, name, ph);
	u32 r[] = { reg, 4 };

	if (!np)
		return NULL;
	of_changeset_add_prop_string(&ocs, np, "compatible", "apple,s5l8940x-pmgr-clk-gate");
	of_changeset_add_prop_u32_array(&ocs, np, "reg", r, 2);
	of_changeset_add_prop_u32(&ocs, np, "#clock-cells", 0);
	of_changeset_add_prop_u32(&ocs, np, "clocks", refclk);
	of_changeset_add_prop_string(&ocs, np, "clock-output-names", name);
	of_changeset_add_prop_u32(&ocs, np, "phandle", ph);
	return np;
}

static struct device_node *amp(struct device_node *i2c, const char *name, u32 addr,
			       u32 ph, u32 gpio, bool keepalive, const char *prefix)
{
	struct device_node *np = node(i2c, name, ph);
	u32 reset[] = { gpio, 5, 1 }, alive[] = { gpio, 6, 0 };

	if (!np)
		return NULL;
	of_changeset_add_prop_string(&ocs, np, "compatible", "cirrus,cs35l19");
	of_changeset_add_prop_u32(&ocs, np, "reg", addr);
	of_changeset_add_prop_u32_array(&ocs, np, "reset-gpios", reset, 3);
	if (keepalive)
		of_changeset_add_prop_u32_array(&ocs, np, "cirrus,keepalive-gpios", alive, 3);
	of_changeset_add_prop_u32(&ocs, np, "clocks", PH_AUDIO);
	of_changeset_add_prop_string(&ocs, np, "clock-names", "mclk");
	of_changeset_add_prop_u32(&ocs, np, "#sound-dai-cells", 0);
	of_changeset_add_prop_string(&ocs, np, "sound-name-prefix", prefix);
	of_changeset_add_prop_u32(&ocs, np, "phandle", ph);
	return np;
}

static int __init p105_audio_dt_init(void)
{
	struct device_node *soc, *i2c, *gpio, *ref, *np;
	static const char * const reg_names[] = {
		"mca", "i2s", "mcs", "acs", "switch", "nco", "cdma" };
	static const char * const clock_names[] = { "mca", "i2s" };
	u32 regs[] = {
		0x4196000, 0x1000, 0x4191000, 0x1000, 0x41e0000, 0x1000,
		0x41a0000, 0x1000, 0xfa01000, 0x1000, 0xf100110, 0x70,
		0x7000000, 0x2d000 };
	u32 clocks[] = { PH_MCA_PS, PH_I2S1_PS };
	u32 amps[] = { PH_SPK_L, PH_SPK_R };
	u32 dma[] = { dma_channel, 0xb };
	int ret = -ENODEV;

	soc = of_find_node_by_path("/soc@30000000");
	i2c = of_find_node_by_path("/soc@30000000/i2c@3200000");
	gpio = of_find_compatible_node(NULL, NULL, "apple,s5l8940x-gpio");
	ref = of_find_node_by_path("/refclk24mhz");
	if (!soc || !i2c || !gpio || !ref || !gpio->phandle || !ref->phandle)
		goto out;

	of_changeset_init(&ocs);
	gate(soc, "mca-ps@f101140", 0xf101140, PH_MCA_PS, ref->phandle);
	gate(soc, "i2s1-ps@f1010a4", 0xf1010a4, PH_I2S1_PS, ref->phandle);

	np = node(soc, "audio-controller@4196000", PH_AUDIO);
	of_changeset_add_prop_string(&ocs, np, "compatible", "apple,s5l8942x-audio");
	of_changeset_add_prop_u32_array(&ocs, np, "reg", regs, ARRAY_SIZE(regs));
	of_changeset_add_prop_string_array(&ocs, np, "reg-names", reg_names, ARRAY_SIZE(reg_names));
	of_changeset_add_prop_u32_array(&ocs, np, "clocks", clocks, 2);
	of_changeset_add_prop_string_array(&ocs, np, "clock-names", clock_names, 2);
	of_changeset_add_prop_u32(&ocs, np, "#clock-cells", 0);
	of_changeset_add_prop_u32(&ocs, np, "#sound-dai-cells", 0);
	of_changeset_add_prop_u32_array(&ocs, np, "apple,dma-channel", dma, 2);
	of_changeset_add_prop_u32(&ocs, np, "apple,nco-reference-hz", 342000000);
	of_changeset_add_prop_u32_array(&ocs, np, "apple,speaker-amps", amps, 2);
	of_changeset_add_prop_u32(&ocs, np, "phandle", PH_AUDIO);

	amp(i2c, "amp@40", 0x40, PH_SPK_L, gpio->phandle, true, "Left");
	amp(i2c, "amp@41", 0x41, PH_SPK_R, gpio->phandle, false, "Right");

	ret = of_changeset_apply(&ocs);
	if (ret)
		of_changeset_destroy(&ocs);
	else
		pr_info("p105-audio-dt: speaker nodes added (CDMA channel %#x)\n", dma_channel);
out:
	of_node_put(soc);
	of_node_put(i2c);
	of_node_put(gpio);
	of_node_put(ref);
	return ret;
}

static void __exit p105_audio_dt_exit(void)
{
	of_changeset_revert(&ocs);
	of_changeset_destroy(&ocs);
}

module_init(p105_audio_dt_init);
module_exit(p105_audio_dt_exit);
MODULE_DESCRIPTION("iPad mini 1 speaker device-tree nodes, for module development");
MODULE_LICENSE("GPL");
