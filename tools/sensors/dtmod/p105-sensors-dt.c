// SPDX-License-Identifier: GPL-2.0-only
/*
 * p105-sensors-dt -- add the motion sensors' bus to a running kernel.
 *
 * Research tool, the counterpart of tools/audio/dtmod: the iPad's third I2C
 * controller (the ADT's arm-io/i2c2, the same Apple block as i2c0) and the
 * three chips the ADT puts on it -- accelerometer,ap3dsh at 0x1d,
 * gyro,ap3gdl at 0x6a, als,tsl2581 at 0x39 -- added with an of_changeset,
 * so the controller and its clients appear the way they would at boot from
 * dts/p105ap.dts and the drivers' DT paths are the ones exercised.
 *
 *   insmod p105-sensors-dt.ko [accel=st,lis3dsh-accel] [frequency=100000]
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/string.h>

static char *accel = "st,lis3dsh-accel";
module_param(accel, charp, 0444);
MODULE_PARM_DESC(accel, "compatible of the accelerometer at 0x1d");

static char *gyro = "st,l3gd20-gyro";
module_param(gyro, charp, 0444);
MODULE_PARM_DESC(gyro, "compatible of the gyroscope at 0x6a");

static unsigned int frequency = 100000;
module_param(frequency, uint, 0444);
MODULE_PARM_DESC(frequency, "the bus's clock in Hz");

static struct of_changeset ocs;

/* (see p105-audio-dt.c) */
static struct device_node *node(struct device_node *parent, const char *full)
{
	struct device_node *np = of_changeset_create_node(&ocs, parent, full);

	if (!np)
		return NULL;
	np->name = kstrndup(full, strcspn(full, "@"), GFP_KERNEL);
	of_node_set_flag(np, OF_OVERLAY);
	return np;
}

static void client(struct device_node *bus, const char *name, u32 addr,
		   const char *compatible)
{
	struct device_node *np = node(bus, name);

	if (!np)
		return;
	of_changeset_add_prop_string(&ocs, np, "compatible", compatible);
	of_changeset_add_prop_u32(&ocs, np, "reg", addr);
}

static int __init p105_sensors_dt_init(void)
{
	struct device_node *soc, *ref, *np;
	u32 reg[] = { 0x3400000, 0x1000 };
	int ret = -ENODEV;

	soc = of_find_node_by_path("/soc@30000000");
	ref = of_find_node_by_path("/refclk24mhz");
	if (!soc || !ref || !ref->phandle)
		goto out;

	of_changeset_init(&ocs);
	/* the ADT's i2c2: reg 0x3400000, AIC interrupt 0x14, its clock gate
	 * (PMGR 0x3f101120) on from the boot */
	np = node(soc, "i2c@3400000");
	ret = -ENOMEM;
	if (!np)
		goto out;
	of_changeset_add_prop_string(&ocs, np, "compatible", "apple,s5l8940x-i2c");
	of_changeset_add_prop_u32_array(&ocs, np, "reg", reg, 2);
	of_changeset_add_prop_u32(&ocs, np, "clock-frequency", frequency);
	of_changeset_add_prop_u32(&ocs, np, "interrupts", 0x14);
	of_changeset_add_prop_u32(&ocs, np, "clocks", ref->phandle);
	of_changeset_add_prop_string(&ocs, np, "clock-names", "i2c");
	of_changeset_add_prop_u32(&ocs, np, "#address-cells", 1);
	of_changeset_add_prop_u32(&ocs, np, "#size-cells", 0);

	client(np, "accelerometer@1d", 0x1d, accel);
	client(np, "gyro@6a", 0x6a, gyro);
	client(np, "light-sensor@39", 0x39, "amstaos,tsl2581");

	ret = of_changeset_apply(&ocs);
	if (ret)
		of_changeset_destroy(&ocs);
	else
		pr_info("p105-sensors-dt: i2c@3400000 added (accelerometer %s)\n", accel);
out:
	of_node_put(soc);
	of_node_put(ref);
	return ret;
}

static void __exit p105_sensors_dt_exit(void)
{
	of_changeset_revert(&ocs);
	of_changeset_destroy(&ocs);
}

module_init(p105_sensors_dt_init);
module_exit(p105_sensors_dt_exit);
MODULE_DESCRIPTION("iPad mini 1 motion sensors' device-tree nodes, for module development");
MODULE_LICENSE("GPL");
