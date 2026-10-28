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
#include <linux/reboot.h>
#include <linux/delay.h>
#include <linux/gpio/driver.h>
#include <linux/apple-pmu-i2c.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include "apple-pmu-d1946-bank0.h"

#if IS_ENABLED(CONFIG_APPLE_PMU_D1946_BANK0_EXPERIMENT)
static bool p105_bank0;
module_param(p105_bank0, bool, 0444);
MODULE_PARM_DESC(p105_bank0, "Experimental D1946 parent bank0 transport; default off");
#endif

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
/*
 * Power-off, the way AppleD1946PMU::_shutdownGated does it (kernelcache
 * 12H321, 0x80ecec80; vtable slot 0x3a4).  In order:
 *
 *   0xd2 = 0                  the backlight's strings off
 *   read 0x01..0x09           the event registers, which clears them, so
 *                             a stale event does not wake it at once
 *   0x12..0x1a = mask table   the events allowed to wake it: iOS's table
 *   0x5b = 0x01               no RTC wake alarm (0x41 with one)
 *   0xa1 = 0
 *   0xaf &= ~0xd0             flags for iBoot; a restart sets 0x90 instead
 *   0x61..0x72: bit 1 off     wake off on every PMU GPIO configured as an
 *                             input (config >= 0x60) -- "cannot turn off GPIO
 *                             wakes" is iOS's message when this fails
 *   0x1b |= 1                 "pmu go stdby": the PMU drops every rail
 *
 * Read on this iPad under Linux before any of it: the masks all 0x00, 0x5b
 * 0x03, 0xaf 0x40, 0x1b 0x00.  iOS's other branch, an external shutdown
 * function, needs function-external_shutdown in the ADT, which P105's PMU
 * does not have.  Hold starts the iPad again, into iOS.
 *
 * All of it but the last write runs in the power-off prepare stage, where
 * regmap may still sleep; the last one runs with interrupts off, straight
 * through the I2C adapter's atomic transfer.
 */
#define D1946_EVENTS        0x01
#define D1946_EVENT_MASKS   0x12
#define D1946_NEVENTS       9
#define D1946_STANDBY       0x1b
#define D1946_ALARM_CTRL    0x5b
#define D1946_GPIO_CFG      0x61
#define D1946_NGPIO         18
#define D1946_GPIO_WAKE     0x02
#define D1946_REG_A1        0xa1
#define D1946_BOOT_FLAGS    0xaf
#define D1946_WLED_ENABLE   0xd2

static const u8 d1946_off_masks[D1946_NEVENTS] = {
    0x88, 0xff, 0xff, 0xff, 0xff, 0xff, 0xae, 0xff, 0xff,
};

#if IS_ENABLED(CONFIG_APPLE_PMU_D1946_BANK0_EXPERIMENT)
static int apple_pmu_d1946_prepare_checked(struct apple_pmu_i2c *pmu)
{
    struct regmap *map = pmu->regmap;
    unsigned int i, v;
    int ret;

    ret = regmap_write(map, D1946_WLED_ENABLE, 0);
    if (ret)
        return ret;
    for (i = 0; i < D1946_NEVENTS; i++) {
        ret = regmap_read(map, D1946_EVENTS + i, &v);
        if (ret)
            return ret;
    }
    for (i = 0; i < D1946_NEVENTS; i++) {
        ret = regmap_write(map, D1946_EVENT_MASKS + i, d1946_off_masks[i]);
        if (ret)
            return ret;
    }
    ret = regmap_write(map, D1946_ALARM_CTRL, 0x01);
    if (ret)
        return ret;
    ret = regmap_write(map, D1946_REG_A1, 0);
    if (ret)
        return ret;
    ret = regmap_update_bits(map, D1946_BOOT_FLAGS, 0xd0, 0);
    if (ret)
        return ret;
    for (i = 0; i < D1946_NGPIO; i++) {
        ret = regmap_read(map, D1946_GPIO_CFG + i, &v);
        if (ret)
            return ret;
        if (v >= 0x60 && (v & D1946_GPIO_WAKE)) {
            ret = regmap_write(map, D1946_GPIO_CFG + i, v & ~D1946_GPIO_WAKE);
            if (ret)
                return ret;
        }
    }
    return 0;
}
#endif

static int apple_pmu_d1946_power_off_prepare(struct sys_off_data *data)
{
    struct regmap *map = ((struct apple_pmu_i2c *)data->cb_data)->regmap;
    unsigned int i, v;
#if IS_ENABLED(CONFIG_APPLE_PMU_D1946_BANK0_EXPERIMENT)
    struct apple_pmu_i2c *pmu = data->cb_data;
    int ret;

    if (pmu->bank0) {
        ret = apple_pmu_d1946_prepare_checked(pmu);
        ret = d1946_bank0_seal(pmu->bank0, ret);
        if (ret)
            dev_err(pmu->dev, "bank0 power-off preparation failed: %d\n", ret);
        return NOTIFY_DONE;
    }
#endif

    regmap_write(map, D1946_WLED_ENABLE, 0);
    for (i = 0; i < D1946_NEVENTS; i++)
        regmap_read(map, D1946_EVENTS + i, &v);
    for (i = 0; i < D1946_NEVENTS; i++)
        regmap_write(map, D1946_EVENT_MASKS + i, d1946_off_masks[i]);
    regmap_write(map, D1946_ALARM_CTRL, 0x01);
    regmap_write(map, D1946_REG_A1, 0);
    regmap_update_bits(map, D1946_BOOT_FLAGS, 0xd0, 0);
    for (i = 0; i < D1946_NGPIO; i++)
        if (!regmap_read(map, D1946_GPIO_CFG + i, &v) && v >= 0x60 && (v & D1946_GPIO_WAKE))
            regmap_write(map, D1946_GPIO_CFG + i, v & ~D1946_GPIO_WAKE);
    return NOTIFY_DONE;
}

static int apple_pmu_d1946_power_off(struct sys_off_data *data)
{
    struct i2c_client *client = to_i2c_client(((struct apple_pmu_i2c *)data->cb_data)->dev);
    int v;
#if IS_ENABLED(CONFIG_APPLE_PMU_D1946_BANK0_EXPERIMENT)
    struct apple_pmu_i2c *pmu = data->cb_data;

    if (pmu->bank0) {
        v = d1946_bank0_power_off_atomic(pmu->bank0);
        if (v) {
            pr_emerg("apple-pmu-i2c: bank0 atomic power-off denied: %d\n", v);
            return NOTIFY_DONE;
        }
        mdelay(1000);
        pr_emerg("apple-pmu-i2c: bank0 standby acknowledged but still running\n");
        return NOTIFY_DONE;
    }
#endif
    v = i2c_smbus_read_byte_data(client, D1946_STANDBY);

    if (v >= 0)
        i2c_smbus_write_byte_data(client, D1946_STANDBY, v | 0x01);
    mdelay(1000);
    pr_emerg("apple-pmu-i2c: the PMU did not power off (standby %d)\n", v);
    return NOTIFY_DONE;
}

/*
 * The PMU's GPIOs, as far as Linux needs them: outputs, their level in bit 1
 * of each pin's configuration register, 0x61 + n.  The ADT names them by
 * that n -- the Wi-Fi chip's REG_ON is GPIO 3 (0x64), the Bluetooth chip's
 * GPIO 2 (0x63) -- and iBoot leaves both configured as outputs, low (0x09).
 * A pin configured as an input (0x60 and up) stays one: this only drives
 * what is already an output, it never turns a pin around.
 */
#define D1946_GPIO_IS_INPUT(cfg)    ((cfg) >= 0x60)
#define D1946_GPIO_LEVEL            0x02

static int apple_pmu_gpio_cfg(struct gpio_chip *gc, unsigned int n, unsigned int *cfg)
{
    struct apple_pmu_i2c *pmu = gpiochip_get_data(gc);

    return regmap_read(pmu->regmap, D1946_GPIO_CFG + n, cfg);
}

static int apple_pmu_gpio_get_direction(struct gpio_chip *gc, unsigned int n)
{
    unsigned int cfg;
    int ret = apple_pmu_gpio_cfg(gc, n, &cfg);

    if (ret)
        return ret;
    return D1946_GPIO_IS_INPUT(cfg) ? GPIO_LINE_DIRECTION_IN : GPIO_LINE_DIRECTION_OUT;
}

static int apple_pmu_gpio_get(struct gpio_chip *gc, unsigned int n)
{
    unsigned int cfg;
    int ret = apple_pmu_gpio_cfg(gc, n, &cfg);

    return ret ? ret : !!(cfg & D1946_GPIO_LEVEL);
}

static void apple_pmu_gpio_set(struct gpio_chip *gc, unsigned int n, int val)
{
    struct apple_pmu_i2c *pmu = gpiochip_get_data(gc);

    regmap_update_bits(pmu->regmap, D1946_GPIO_CFG + n, D1946_GPIO_LEVEL,
                       val ? D1946_GPIO_LEVEL : 0);
}

static int apple_pmu_gpio_direction_output(struct gpio_chip *gc, unsigned int n, int val)
{
    struct apple_pmu_i2c *pmu = gpiochip_get_data(gc);
    unsigned int cfg;
    int ret = apple_pmu_gpio_cfg(gc, n, &cfg);

    if (ret)
        return ret;
    if (D1946_GPIO_IS_INPUT(cfg))
        return -EPERM;
    return regmap_update_bits(pmu->regmap, D1946_GPIO_CFG + n,
                              D1946_GPIO_LEVEL, val ? D1946_GPIO_LEVEL : 0);
}

static int apple_pmu_gpio_direction_input(struct gpio_chip *gc, unsigned int n)
{
    int direction = apple_pmu_gpio_get_direction(gc, n);

    if (direction < 0)
        return direction;
    return direction == GPIO_LINE_DIRECTION_IN ? 0 : -EPERM;
}

static int apple_pmu_gpio_register(struct apple_pmu_i2c *pmu)
{
    struct gpio_chip *gc = devm_kzalloc(pmu->dev, sizeof(*gc), GFP_KERNEL);

    if (!gc)
        return -ENOMEM;
    gc->label = "apple-pmu-gpio";
    gc->parent = pmu->dev;
    gc->owner = THIS_MODULE;
    gc->base = -1;
    gc->ngpio = D1946_NGPIO;
    gc->can_sleep = true;
    gc->get_direction = apple_pmu_gpio_get_direction;
    gc->direction_input = apple_pmu_gpio_direction_input;
    gc->direction_output = apple_pmu_gpio_direction_output;
    gc->get = apple_pmu_gpio_get;
    gc->set = apple_pmu_gpio_set;
    return devm_gpiochip_add_data(pmu->dev, gc, pmu);
}

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

#if IS_ENABLED(CONFIG_APPLE_PMU_D1946_BANK0_EXPERIMENT)
    if (p105_bank0) {
        if (info != &apple_pmu_i2c_d1946_info ||
            !of_machine_is_compatible("apple,p105ap"))
            return -EOPNOTSUPP;
        pmu->bank0 = devm_kzalloc(&i2c->dev, sizeof(*pmu->bank0), GFP_KERNEL);
        if (!pmu->bank0)
            return -ENOMEM;
        ret = d1946_bank0_init(pmu->bank0, i2c);
        if (ret)
            return ret;
        d1946_bank0_config(pmu->bank0, &pmu->config);
        pmu->regmap = devm_regmap_init(&i2c->dev, d1946_bank0_regmap_bus(),
                                       pmu->bank0, &pmu->config);
    } else
#endif
        pmu->regmap = devm_regmap_init_i2c(i2c, &pmu->config);
    if(IS_ERR(pmu->regmap)) {
        ret = PTR_ERR(pmu->regmap);
        dev_err(&i2c->dev, "failed to create regmap: %d.\n", ret);
        return ret;
    }

#if IS_ENABLED(CONFIG_APPLE_PMU_D1946_BANK0_EXPERIMENT)
    if (pmu->bank0) {
        /* Registered before children/GPIO/sys-off: all drain before bus stop. */
        ret = devm_add_action_or_reset(&i2c->dev, d1946_bank0_stop, pmu->bank0);
        if (ret)
            return ret;
    }
#endif

    /* Do not dump the register file during probe: the D1946 IRQ handler
     * consumes event registers 0x01..0x09. Diagnostic reads risk clearing
     * pending events before child drivers can handle them. */

    if (info == &apple_pmu_i2c_d1946_info && of_device_is_system_power_controller(node)) {
        ret = devm_register_sys_off_handler(&i2c->dev, SYS_OFF_MODE_POWER_OFF_PREPARE,
                                            SYS_OFF_PRIO_DEFAULT,
                                            apple_pmu_d1946_power_off_prepare, pmu);
        if (!ret)
            ret = devm_register_sys_off_handler(&i2c->dev, SYS_OFF_MODE_POWER_OFF,
                                                SYS_OFF_PRIO_DEFAULT,
                                                apple_pmu_d1946_power_off, pmu);
        if (ret) {
#if IS_ENABLED(CONFIG_APPLE_PMU_D1946_BANK0_EXPERIMENT)
            if (pmu->bank0)
                return ret;
#endif
            dev_warn(&i2c->dev, "no power-off: %d\n", ret);
        }
    }

    if (info == &apple_pmu_i2c_d1946_info && of_property_read_bool(node, "gpio-controller")) {
        ret = apple_pmu_gpio_register(pmu);
        if (ret) {
#if IS_ENABLED(CONFIG_APPLE_PMU_D1946_BANK0_EXPERIMENT)
            if (pmu->bank0)
                return ret;
#endif
            dev_warn(&i2c->dev, "no GPIOs: %d\n", ret);
        }
    }

#if IS_ENABLED(CONFIG_APPLE_PMU_D1946_BANK0_EXPERIMENT)
    if (pmu->bank0) {
        ret = devm_of_platform_populate(&i2c->dev);
        if (ret) {
            /* populate may have created a partial set without a devres action. */
            of_platform_depopulate(&i2c->dev);
            return ret;
        }
        return 0;
    }
#endif
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


#if IS_ENABLED(CONFIG_APPLE_PMU_D1946_BANK0_EXPERIMENT)
/* Probe-time direct child only. The managed link keeps parent devres alive until
 * child probe unwind/remove drains its users. No parent lock: children may first
 * probe inside OF population while the parent is still probing; defer instead.
 * This observes real transport identity/health, not analog charger permission. */
struct regmap *apple_pmu_d1946_bank0_get_for_child(struct device *child)
{
    struct device *parent;
    struct i2c_client *client;
    struct device_link *link;
    const struct of_device_id *match;
    struct apple_pmu_i2c *pmu;
    enum device_link_state status;
    int ret;

    if (IS_ERR_OR_NULL(child) || !dev_is_platform(child) ||
        READ_ONCE(child->links.status) != DL_DEV_PROBING)
        return ERR_PTR(-EINVAL);
    parent = child->parent;
    if (IS_ERR_OR_NULL(parent) || parent == child)
        return ERR_PTR(-EINVAL);
    client = i2c_verify_client(parent);
    if (!client)
        return ERR_PTR(-ENODEV);
    if (READ_ONCE(parent->driver) != &apple_pmu_i2c_driver.driver)
        return ERR_PTR(READ_ONCE(parent->driver) ? -ENODEV : -EPROBE_DEFER);
    if (READ_ONCE(parent->links.status) != DL_DEV_DRIVER_BOUND)
        return ERR_PTR(-EPROBE_DEFER);
    match = of_match_device(apple_pmu_i2c_of_match, parent);
    if (!match || match->data != &apple_pmu_i2c_d1946_info ||
        !of_machine_is_compatible("apple,p105ap"))
        return ERR_PTR(-EOPNOTSUPP);

    link = device_link_add(child, parent, DL_FLAG_AUTOREMOVE_CONSUMER);
    if (!link)
        return ERR_PTR(-EPROBE_DEFER);
    status = READ_ONCE(link->status);
    if ((status != DL_STATE_CONSUMER_PROBE && status != DL_STATE_ACTIVE) ||
        READ_ONCE(parent->links.status) != DL_DEV_DRIVER_BOUND ||
        READ_ONCE(parent->driver) != &apple_pmu_i2c_driver.driver)
        return ERR_PTR(-EPROBE_DEFER);
    /* Only a correctly bound, lifetime-guarded parent may expose its drvdata. */
    pmu = dev_get_drvdata(parent);
    if (IS_ERR_OR_NULL(pmu) || pmu->dev != parent ||
        IS_ERR_OR_NULL(pmu->regmap))
        return ERR_PTR(-ENODEV);
    if (!pmu->bank0)
        return ERR_PTR(-EOPNOTSUPP);
    ret = d1946_bank0_check_running(pmu->bank0, client);
    if (ret)
        return ERR_PTR(ret);
    if (dev_get_regmap(parent, NULL) != pmu->regmap)
        return ERR_PTR(-ENODEV);
    /* State can change after this snapshot; every regmap callback rechecks its
     * own admission under the existing map/adapter locks before data access. */
    return pmu->regmap;
}
EXPORT_SYMBOL_GPL(apple_pmu_d1946_bank0_get_for_child);
#endif

static int __init apple_pmu_i2c_init(void)
{
    return i2c_add_driver(&apple_pmu_i2c_driver);
}

subsys_initcall(apple_pmu_i2c_init);
