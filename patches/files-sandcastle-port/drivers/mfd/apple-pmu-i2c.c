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
/*
 * The PMU on this board is a D1946 -- the ADT node is "pmu,d1946" and the only
 * PMU driver in the 12H321 cache is AppleD1946PMU -- and it takes an 8-bit
 * register address, not 16.  iBoot settles it: its PMU register write (iBEC
 * 2261.30.37, 0xbe6c) builds a two-byte buffer of [reg, value] and calls
 * i2c_write with txlen 2 to address 0x78, and the read beside it uses txlen 1.
 *
 * We had this node on the d2333 profile, so every access sent two address
 * bytes.  Asking for "register 0x020c" put 0x02 on the bus as the register
 * number and 0x0c as data -- which is why every register in 0x00..0xff read
 * back the same byte and why none of the rail writes ever stuck.
 *
 * It also explains the kernelcache offsets, which are all single-byte:
 * _setGPIOFunction uses gpio + 0x61, _setLDO uses ldo + 0x2f with a shared
 * enable at 0x7c.  And it means the ADT's 0x020c and 0x0213 are not addresses
 * at all but flags 0x02 plus an index -- LDO 12 and LDO 19, both inside
 * _setLDO's bounds check of 0x16.
 */
static const struct apple_pmu_i2c_info apple_pmu_i2c_d1946_info = { .reg_bits = 8, .val_bits = 8 };
static const struct of_device_id apple_pmu_i2c_of_match[] = {
    { .compatible = "apple,pmu-d1946", .data = &apple_pmu_i2c_d1946_info },
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
    if(IS_ERR(pmu->regmap)) {
        ret = PTR_ERR(pmu->regmap);
        dev_err(&i2c->dev, "failed to create regmap: %d.\n", ret);
        return ret;
    }

    /* Bring-up: with the bus finally working, dump the chip once.  This is
     * cheap now -- it only cost eighteen seconds of boot back when every
     * transfer ran into the 100 ms timeout.
     *
     * What we are looking for: the ADT calls power_ana and power_ldo with
     * 0x020c and 0x0213, which this tree reads as register addresses.  Both
     * read 0x00 and stay 0x00 after a write, which is how a register that
     * does not exist behaves.  AppleD1946PMU::_setLDO takes an LDO *index*
     * (bounds-checked at 0x16) whose voltage register is index + 0x2f, with
     * a shared enable bit at 0x7c for a few of them; read as index + flags,
     * 0x20c and 0x213 are LDO 12 and LDO 19, both in range -- so the real
     * registers would be 0x3b and 0x42.  The dump settles it. */
    {
        static const struct { unsigned first, last; } banks[] = {
            { 0x00, 0xff },
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
