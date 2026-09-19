/*
 * Driver for I2C-based PMUs found on mobile devices with S5L8940X (Apple A5) SoCs.
 *
 * Copyright (C) 2020 Corellium LLC
 */

#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/apple-pmu-i2c.h>

struct apple_pmu_i2c_info {
    unsigned reg_bits, val_bits;
};

static const struct apple_pmu_i2c_info apple_pmu_i2c_d2333_info = { .reg_bits = 16, .val_bits = 8 };
static const struct apple_pmu_i2c_info apple_pmu_i2c_chestnut_info = { .reg_bits = 8, .val_bits = 8 };
static const struct of_device_id apple_pmu_i2c_of_match[] = {
    { .compatible = "apple,pmu-d2333", .data = &apple_pmu_i2c_d2333_info },
    { .compatible = "apple,pmu-chestnut", .data = &apple_pmu_i2c_chestnut_info },
    { },
};

static int apple_pmu_i2c_probe(struct i2c_client *i2c)
{
    struct device_node *node = i2c->dev.of_node, *child;
    const struct of_device_id *of_id;
    const struct apple_pmu_i2c_info *info;
    struct apple_pmu_i2c *pmu;
    int ret;

    pmu = devm_kzalloc(&i2c->dev, sizeof(*pmu), GFP_KERNEL);
    if(!pmu)
        return -ENOMEM;

    of_id = of_match_device(apple_pmu_i2c_of_match, &i2c->dev);
    if(!of_id) {
        dev_err(&i2c->dev, "failed to match device.\n");
        return -ENODEV;
    }
    info = of_id->data;

    pmu->dev = &i2c->dev;
    i2c_set_clientdata(i2c, pmu);

    pmu->config.reg_bits = info->reg_bits;
    pmu->config.val_bits = info->val_bits;
    pmu->config.max_register = (1u << pmu->config.reg_bits) - 1u;

    pmu->regmap = devm_regmap_init_i2c(i2c, &pmu->config);

    /* Bring-up: dump the PMU once so we stop inferring its state.  The
     * digitizer is powered through this chip and every "enable" so far has
     * been a guess at what an ADT argument means -- 0x20c/0x213 were read as
     * register addresses, but AppleD1946PMU::_setLDO takes an LDO *index*
     * (<= 0x16) whose registers live at index + 0x2f, so those may be two
     * unrelated registers we have been poking.  Print the low bank (LDO
     * voltages at 0x2f.., the shared enable at 0x7c, GPIOs at 0x61..0x72)
     * and the 0x200 bank, and compare against what the digitizer needs. */
    if (!IS_ERR(pmu->regmap)) {
        static const struct { unsigned first, last; } banks[] = {
            { 0x00, 0x7f }, { 0x200, 0x21f },
        };
        unsigned b, r, c;
        char line[80];

        for (b = 0; b < ARRAY_SIZE(banks); b++)
            for (r = banks[b].first; r <= banks[b].last; r += 16) {
                int n = 0;
                for (c = 0; c < 16 && r + c <= banks[b].last; c++) {
                    unsigned int v = 0;
                    n += scnprintf(line + n, sizeof(line) - n, "%s%02x",
                                   c ? " " : "",
                                   regmap_read(pmu->regmap, r + c, &v) ? 0xff : (v & 0xff));
                }
                dev_info(&i2c->dev, "PMU %03x: %s\n", r, line);
            }
    }

    if(IS_ERR(pmu->regmap)) {
        ret = PTR_ERR(pmu->regmap);
        dev_err(&i2c->dev, "failed to create regmap: %d.\n", ret);
        return ret;
    }

    for_each_child_of_node(node, child)
        of_platform_device_create(child, NULL, &i2c->dev);

    return 0;
}

static struct i2c_driver apple_pmu_i2c_driver = {
    .driver = {
        .name = "apple-pmu-i2c",
        .of_match_table = of_match_ptr(apple_pmu_i2c_of_match),
    },
    .probe = apple_pmu_i2c_probe,
};

static int __init apple_pmu_i2c_init(void)
{
    return i2c_add_driver(&apple_pmu_i2c_driver);
}

subsys_initcall(apple_pmu_i2c_init);
