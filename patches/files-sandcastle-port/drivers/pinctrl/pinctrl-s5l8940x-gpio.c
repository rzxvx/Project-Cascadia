/*
 * S5L8940X (Apple A5) SoC pinctrl+GPIO+external IRQ driver
 *
 * Copyright (C) 2020 Corellium LLC
 *
 * Based on: pinctrl-pistachio.c
 * Copyright (C) 2014 Imagination Technologies Ltd.
 * Copyright (C) 2014 Google, Inc.
 */

#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "pinctrl-utils.h"
#include "core.h"
#include "devicetree.h"
#include "pinconf.h"
#include "pinmux.h"

struct apple_s5l8940x_gpio_pincfg {
    uint8_t irqtype;
    uint8_t stat;
};

#define PINCFG_STAT_OUTVAL      0x01
#define PINCFG_STAT_OUTEN       0x02
#define PINCFG_STAT_PERIPH      0x20
#define PINCFG_STAT_IRQEN       0x80

struct apple_s5l8940x_gpio_pinctrl {
    struct device *dev;
    struct pinctrl_dev *pctldev;

    unsigned int pin_base;
    const char *pin_prefix;
    unsigned int npins;
    struct pinctrl_pin_desc *pins;
    struct apple_s5l8940x_gpio_pincfg *pin_cfgs;
    const char **pin_names;
    unsigned *pin_nums;

    void __iomem *base;
    unsigned int nirqgrps;
    int *irqs;

    struct pinctrl_desc pinctrl_desc;
    struct gpio_chip gpio_chip;
    struct irq_chip irq_chip;
};

#define REG_GPIO(x)             (4 * (x))
#define  REG_GPIOx_DATA         (1 << 0)
#define  REG_GPIOx_IRQ_MASK     (7 << 1)
#define    REG_GPIOx_IRQ_OUT    (1 << 1)
#define    REG_GPIOx_IRQ_HI     (2 << 1)
#define    REG_GPIOx_IRQ_LO     (3 << 1)
#define    REG_GPIOx_IRQ_UP     (4 << 1)
#define    REG_GPIOx_IRQ_DN     (5 << 1)
#define    REG_GPIOx_IRQ_ANY    (6 << 1)
#define    REG_GPIOx_IRQ_OFF    (7 << 1)
#define  REG_GPIOx_PERIPH       (1 << 5)
#define  REG_GPIOx_CFG_DONE     (1 << 9)
#define  REG_GPIOx_GRP_MASK     (7 << 16)
#define    REG_GPIOx_GRP_SHIFT  16
/*
 * Interrupt block.  The driver came from Sandcastle (A10), whose GPIO block
 * gives every interrupt group its own 0x40 window of pending words at 0x800.
 * The A5's is laid out differently, and XNU says how: AppleS5L8930XGPIOIC,
 * the class that matches "gpio,s5l8930x" -- which this ADT's gpio node
 * lists -- in com.apple.driver.AppleS5L8930X (12H321):
 *
 *   start              0x800 + 4*w <- ~0     every pin's interrupt off
 *                      0x880 + 4*w <- ~0     every status bit cleared
 *                      0xc48       <- 1      unless "no-npl-mode" (P105: absent)
 *   handleInterrupt    0xc00                 one bit per word that has work
 *                      0x880 + 4*w           status; write 1 to clear -- before
 *                                            the handler for an edge pin,
 *                                            after it for a level pin
 *   enableVector       0x840 + 4*w <- bit    (level pin: clear 0x880 first)
 *   disableVectorHard  0x800 + 4*w <- bit
 *
 * w is pin / 32 and the bit is pin % 32; the ADT's "#interrupt-groups" = 8 is
 * nothing more than the number of words.  So 0x800, which this driver read as
 * "pending" and wrote to "ack", is the DISABLE register: the handler could
 * never see a status bit, the ack switched the pin off, and nothing ever wrote
 * the enable.  No GPIO interrupt -- the digitizer's ATTN included -- could
 * reach Linux on this SoC, whatever the pin did.
 */
#define REG_IRQ_DISABLE(x)      (0x800 + 4 * ((x) >> 5))
#define REG_IRQ_ENABLE(x)       (0x840 + 4 * ((x) >> 5))
#define REG_IRQ_STATUS(x)       (0x880 + 4 * ((x) >> 5))
#define REG_IRQ_SUMMARY         0xc00
#define REG_NPL_MODE            0xc48
#define IRQ_NWORDS              8
#define REG_LOCK                0xC50

static void apple_s5l8940x_gpio_set_reg(struct apple_s5l8940x_gpio_pinctrl *pctl, unsigned pin, uint32_t clr, uint32_t set)
{
    void __iomem *ppin = pctl->base + pin * 4;
    uint32_t prev, cfg;

    prev = readl(ppin);
    cfg = (prev & ~clr) | set;

    if(cfg & REG_GPIOx_CFG_DONE) {
        if(!(prev & REG_GPIOx_CFG_DONE))
            writel(cfg & ~REG_GPIOx_CFG_DONE, ppin);
    } else
        writel(prev & ~REG_GPIOx_CFG_DONE, ppin);
    writel(cfg, ppin);
}

static void apple_s5l8940x_gpio_refresh_reg(struct apple_s5l8940x_gpio_pinctrl *pctl, unsigned pin)
{
    struct apple_s5l8940x_gpio_pincfg *pincfg = &pctl->pin_cfgs[pin];

    if(pincfg->stat & PINCFG_STAT_PERIPH) {
        apple_s5l8940x_gpio_set_reg(pctl, pin, REG_GPIOx_IRQ_MASK | REG_GPIOx_DATA, REG_GPIOx_PERIPH | REG_GPIOx_CFG_DONE | (pincfg->stat & PINCFG_STAT_OUTVAL));
        return;
    }

    if(pincfg->stat & PINCFG_STAT_OUTEN) {
        apple_s5l8940x_gpio_set_reg(pctl, pin, REG_GPIOx_IRQ_MASK | REG_GPIOx_DATA | REG_GPIOx_PERIPH, REG_GPIOx_CFG_DONE | REG_GPIOx_IRQ_OUT | (pincfg->stat & PINCFG_STAT_OUTVAL));
        return;
    }

    if(pincfg->stat & PINCFG_STAT_IRQEN) {
        apple_s5l8940x_gpio_set_reg(pctl, pin, REG_GPIOx_IRQ_MASK | REG_GPIOx_DATA | REG_GPIOx_PERIPH, REG_GPIOx_CFG_DONE | pincfg->irqtype | (pincfg->stat & PINCFG_STAT_OUTVAL));
        return;
    }

    apple_s5l8940x_gpio_set_reg(pctl, pin, REG_GPIOx_IRQ_MASK | REG_GPIOx_DATA | REG_GPIOx_PERIPH, REG_GPIOx_CFG_DONE | REG_GPIOx_IRQ_OFF | (pincfg->stat & PINCFG_STAT_OUTVAL));
}

static uint32_t apple_s5l8940x_gpio_get_reg(struct apple_s5l8940x_gpio_pinctrl *pctl, unsigned pin)
{
    return readl(pctl->base + pin * 4);
}

static void apple_s5l8940x_gpio_init_reg(struct apple_s5l8940x_gpio_pinctrl *pctl, unsigned pin)
{
    struct apple_s5l8940x_gpio_pincfg *pincfg = &pctl->pin_cfgs[pin];
    uint32_t reg = apple_s5l8940x_gpio_get_reg(pctl, pin);

    pincfg->irqtype = 0;
    if(reg & REG_GPIOx_PERIPH) {
        pincfg->stat = PINCFG_STAT_PERIPH;
    } else if((reg & REG_GPIOx_IRQ_MASK) == REG_GPIOx_IRQ_OUT) {
        pincfg->stat = PINCFG_STAT_OUTEN | (reg & PINCFG_STAT_OUTVAL);
    } else if((reg & REG_GPIOx_IRQ_MASK) == REG_GPIOx_IRQ_OFF || !(reg & REG_GPIOx_IRQ_MASK)) {
        pincfg->stat = 0;
    } else {
        pincfg->irqtype = reg & REG_GPIOx_IRQ_MASK;
        pincfg->stat = PINCFG_STAT_IRQEN;
    }
}

/* Pin controller functions */

static int apple_s5l8940x_gpio_pinctrl_get_groups_count(struct pinctrl_dev *pctldev)
{
    struct apple_s5l8940x_gpio_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);

    return pctl->npins;
}

static const char *apple_s5l8940x_gpio_pinctrl_get_group_name(struct pinctrl_dev *pctldev, unsigned group)
{
    struct apple_s5l8940x_gpio_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);

    return pctl->pins[group].name;
}

static int apple_s5l8940x_gpio_pinctrl_get_group_pins(struct pinctrl_dev *pctldev, unsigned group, const unsigned **pins, unsigned *num_pins)
{
    struct apple_s5l8940x_gpio_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);

    *pins = &pctl->pin_nums[group];
    *num_pins = 1;

    return 0;
}

static const struct pinctrl_ops apple_s5l8940x_gpio_pinctrl_ops = {
    .get_groups_count = apple_s5l8940x_gpio_pinctrl_get_groups_count,
    .get_group_name = apple_s5l8940x_gpio_pinctrl_get_group_name,
    .get_group_pins = apple_s5l8940x_gpio_pinctrl_get_group_pins,
    .dt_node_to_map = pinconf_generic_dt_node_to_map_pin,
    .dt_free_map = pinctrl_utils_free_map,
};

/* Pin multiplexer functions */

static int apple_s5l8940x_gpio_pinmux_get_functions_count(struct pinctrl_dev *pctldev)
{
    return 2;
}

static const char *apple_s5l8940x_gpio_pinmux_get_function_name(struct pinctrl_dev *pctldev, unsigned func)
{
    return func ? "periph" : "gpio";
}

static int apple_s5l8940x_gpio_pinmux_get_function_groups(struct pinctrl_dev *pctldev, unsigned func, const char * const **groups, unsigned * const num_groups)
{
    struct apple_s5l8940x_gpio_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);

    *groups = pctl->pin_names;
    *num_groups = pctl->npins;
    return 0;
}

static int apple_s5l8940x_gpio_pinmux_enable(struct pinctrl_dev *pctldev, unsigned func, unsigned group)
{
    struct apple_s5l8940x_gpio_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);

    if(func)
        pctl->pin_cfgs[group].stat |= PINCFG_STAT_PERIPH;
    else
        pctl->pin_cfgs[group].stat &= ~PINCFG_STAT_PERIPH;
    apple_s5l8940x_gpio_refresh_reg(pctl, group);

    return 0;
}

static const struct pinmux_ops apple_s5l8940x_gpio_pinmux_ops = {
    .get_functions_count = apple_s5l8940x_gpio_pinmux_get_functions_count,
    .get_function_name = apple_s5l8940x_gpio_pinmux_get_function_name,
    .get_function_groups = apple_s5l8940x_gpio_pinmux_get_function_groups,
    .set_mux = apple_s5l8940x_gpio_pinmux_enable,
};

/* Pin configuration functions */

static int apple_s5l8940x_gpio_pinconf_get(struct pinctrl_dev *pctldev, unsigned pin, unsigned long *config)
{
    return -ENOTSUPP;
}

static int apple_s5l8940x_gpio_pinconf_set(struct pinctrl_dev *pctldev, unsigned pin, unsigned long *configs, unsigned num_configs)
{
    return -ENOTSUPP;
}

static const struct pinconf_ops apple_s5l8940x_gpio_pinconf_ops = {
    .pin_config_get = apple_s5l8940x_gpio_pinconf_get,
    .pin_config_set = apple_s5l8940x_gpio_pinconf_set,
    .is_generic = true,
};

/* GPIO chip functions */

static int apple_s5l8940x_gpio_gpio_get_direction(struct gpio_chip *chip, unsigned offset)
{
    struct apple_s5l8940x_gpio_pinctrl *pctl = gpiochip_get_data(chip);

    return !(pctl->pin_cfgs[offset].stat & PINCFG_STAT_OUTEN);
}

static int apple_s5l8940x_gpio_gpio_get(struct gpio_chip *chip, unsigned offset)
{
    struct apple_s5l8940x_gpio_pinctrl *pctl = gpiochip_get_data(chip);
    uint32_t reg;

    reg = apple_s5l8940x_gpio_get_reg(pctl, offset);
    return !!(reg & REG_GPIOx_DATA);
}

static void apple_s5l8940x_gpio_gpio_set(struct gpio_chip *chip, unsigned offset, int value)
{
    struct apple_s5l8940x_gpio_pinctrl *pctl = gpiochip_get_data(chip);

    if(value)
        pctl->pin_cfgs[offset].stat |= PINCFG_STAT_OUTVAL;
    else
        pctl->pin_cfgs[offset].stat &= ~PINCFG_STAT_OUTVAL;
    apple_s5l8940x_gpio_refresh_reg(pctl, offset);
}

static int apple_s5l8940x_gpio_gpio_direction_input(struct gpio_chip *chip, unsigned offset)
{
    struct apple_s5l8940x_gpio_pinctrl *pctl = gpiochip_get_data(chip);

    pctl->pin_cfgs[offset].stat &= ~PINCFG_STAT_OUTEN;
    apple_s5l8940x_gpio_refresh_reg(pctl, offset);
    return 0;
}

static int apple_s5l8940x_gpio_gpio_direction_output(struct gpio_chip *chip, unsigned offset, int value)
{
    struct apple_s5l8940x_gpio_pinctrl *pctl = gpiochip_get_data(chip);

    if(value) {
        pctl->pin_cfgs[offset].stat &= ~PINCFG_STAT_PERIPH;
        pctl->pin_cfgs[offset].stat |= PINCFG_STAT_OUTEN | PINCFG_STAT_OUTVAL;
    } else {
        pctl->pin_cfgs[offset].stat &= ~(PINCFG_STAT_OUTVAL | PINCFG_STAT_PERIPH);
        pctl->pin_cfgs[offset].stat |= PINCFG_STAT_OUTEN;
    }
    apple_s5l8940x_gpio_refresh_reg(pctl, offset);
    return 0;
}

/* IRQ chip functions */

static void apple_s5l8940x_gpio_gpio_irq_ack(struct irq_data *data)
{
    struct apple_s5l8940x_gpio_pinctrl *pctl = gpiochip_get_data(irq_data_get_irq_chip_data(data));

    writel(BIT(data->hwirq & 31), pctl->base + REG_IRQ_STATUS(data->hwirq));
}

static void apple_s5l8940x_gpio_gpio_irq_mask(struct irq_data *data)
{
    struct apple_s5l8940x_gpio_pinctrl *pctl = gpiochip_get_data(irq_data_get_irq_chip_data(data));

    /* XNU's disableVectorHard: the pin keeps its trigger mode, the block
     * stops passing it on. */
    writel(BIT(data->hwirq & 31), pctl->base + REG_IRQ_DISABLE(data->hwirq));
}

static void apple_s5l8940x_gpio_gpio_irq_unmask(struct irq_data *data)
{
    struct apple_s5l8940x_gpio_pinctrl *pctl = gpiochip_get_data(irq_data_get_irq_chip_data(data));
    struct apple_s5l8940x_gpio_pincfg *pincfg = &pctl->pin_cfgs[data->hwirq];
    u32 bit = BIT(data->hwirq & 31);

    /* The pin has to be in its trigger mode ... */
    if(!(pincfg->stat & PINCFG_STAT_IRQEN)) {
        pincfg->stat |= PINCFG_STAT_IRQEN;
        apple_s5l8940x_gpio_refresh_reg(pctl, data->hwirq);
    }
    /* ... and then XNU's enableVector: a level pin's stale status is cleared
     * first, then the block lets the pin through. */
    if(pincfg->irqtype == REG_GPIOx_IRQ_HI || pincfg->irqtype == REG_GPIOx_IRQ_LO)
        writel(bit, pctl->base + REG_IRQ_STATUS(data->hwirq));
    writel(bit, pctl->base + REG_IRQ_ENABLE(data->hwirq));
}

static unsigned int apple_s5l8940x_gpio_gpio_irq_startup(struct irq_data *data)
{
    struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
    struct apple_s5l8940x_gpio_pinctrl *pctl = gpiochip_get_data(chip);
    unsigned irqgrp = 0;

    apple_s5l8940x_gpio_set_reg(pctl, data->hwirq, REG_GPIOx_GRP_MASK, irqgrp << REG_GPIOx_GRP_SHIFT);

    apple_s5l8940x_gpio_gpio_direction_input(chip, data->hwirq);
    apple_s5l8940x_gpio_gpio_irq_unmask(data);

    return 0;
}

static int apple_s5l8940x_gpio_gpio_irq_set_type(struct irq_data *data, unsigned int type)
{
    struct apple_s5l8940x_gpio_pinctrl *pctl = gpiochip_get_data(irq_data_get_irq_chip_data(data));

    switch(type & IRQ_TYPE_SENSE_MASK) {
    case IRQ_TYPE_EDGE_RISING:
        pctl->pin_cfgs[data->hwirq].irqtype = REG_GPIOx_IRQ_UP;
        break;
    case IRQ_TYPE_EDGE_FALLING:
        pctl->pin_cfgs[data->hwirq].irqtype = REG_GPIOx_IRQ_DN;
        break;
    case IRQ_TYPE_EDGE_BOTH:
        pctl->pin_cfgs[data->hwirq].irqtype = REG_GPIOx_IRQ_ANY;
        break;
    case IRQ_TYPE_LEVEL_HIGH:
        pctl->pin_cfgs[data->hwirq].irqtype = REG_GPIOx_IRQ_HI;
        break;
    case IRQ_TYPE_LEVEL_LOW:
        pctl->pin_cfgs[data->hwirq].irqtype = REG_GPIOx_IRQ_LO;
        break;
    default:
        return -EINVAL;
    }

    apple_s5l8940x_gpio_refresh_reg(pctl, data->hwirq);

    if(type & IRQ_TYPE_LEVEL_MASK)
        irq_set_handler_locked(data, handle_level_irq);
    else
        irq_set_handler_locked(data, handle_edge_irq);
    return 0;
}

static void apple_s5l8940x_gpio_gpio_irq_handler(struct irq_desc *desc)
{
    struct gpio_chip *gc = irq_desc_get_handler_data(desc);
    struct apple_s5l8940x_gpio_pinctrl *pctl = gpiochip_get_data(gc);
    struct irq_chip *chip = irq_desc_get_chip(desc);
    unsigned long summary, status;
    unsigned w, b, pin, pass;

    chained_irq_enter(chip, desc);
    /* XNU's handleInterrupt: the summary names the words with work, the word
     * names the pins.  Bounded, where XNU's loop is not: a bit that will not
     * clear should cost a warning, not the only CPU this kernel runs on. */
    for(pass = 0; pass < 32; pass++) {
        summary = readl(pctl->base + REG_IRQ_SUMMARY) & (BIT(IRQ_NWORDS) - 1);
        if(!summary)
            break;
        for_each_set_bit(w, &summary, IRQ_NWORDS) {
            status = readl(pctl->base + REG_IRQ_STATUS(w * 32));
            for_each_set_bit(b, &status, 32) {
                pin = w * 32 + b;
                if(pin < pctl->npins && !generic_handle_domain_irq(gc->irq.domain, pin))
                    continue;
                /* Nobody in Linux asked for this pin -- iBoot armed it, or it
                 * lies past the last pin.  Its status would hold the AIC line
                 * up for good, so switch it off and clear it. */
                writel(BIT(b), pctl->base + REG_IRQ_DISABLE(pin));
                writel(BIT(b), pctl->base + REG_IRQ_STATUS(pin));
                dev_warn_ratelimited(pctl->dev, "stray interrupt on pin %u, disabled\n", pin);
            }
        }
    }
    if(pass == 32)
        dev_warn_ratelimited(pctl->dev, "interrupt summary will not clear: %08x\n",
                             readl(pctl->base + REG_IRQ_SUMMARY));
    chained_irq_exit(chip, desc);
}

/* Probe & register */

static int apple_s5l8940x_gpio_gpio_register(struct apple_s5l8940x_gpio_pinctrl *pctl)
{
    struct device_node *node = pctl->dev->of_node;
    int ret = 0;

    if(!of_find_property(node, "gpio-controller", NULL)) {
        dev_err(pctl->dev, "Hx GPIO must have 'gpio-controller' property.\n");
        return -ENODEV;
    }

    pctl->gpio_chip.label = pctl->pin_prefix;
    pctl->gpio_chip.request = gpiochip_generic_request;
    pctl->gpio_chip.free = gpiochip_generic_free;
    pctl->gpio_chip.get_direction = apple_s5l8940x_gpio_gpio_get_direction;
    pctl->gpio_chip.direction_input = apple_s5l8940x_gpio_gpio_direction_input;
    pctl->gpio_chip.direction_output = apple_s5l8940x_gpio_gpio_direction_output;
    pctl->gpio_chip.get = apple_s5l8940x_gpio_gpio_get;
    pctl->gpio_chip.set = apple_s5l8940x_gpio_gpio_set;
    pctl->gpio_chip.base = 0;
    pctl->gpio_chip.ngpio = pctl->npins;
    pctl->gpio_chip.parent = pctl->dev;

    pctl->irq_chip.name = pctl->pin_prefix;
    pctl->irq_chip.irq_startup = apple_s5l8940x_gpio_gpio_irq_startup;
    pctl->irq_chip.irq_ack = apple_s5l8940x_gpio_gpio_irq_ack;
    pctl->irq_chip.irq_mask = apple_s5l8940x_gpio_gpio_irq_mask;
    pctl->irq_chip.irq_unmask = apple_s5l8940x_gpio_gpio_irq_unmask;
    pctl->irq_chip.irq_set_type = apple_s5l8940x_gpio_gpio_irq_set_type;

    {
        struct gpio_irq_chip *girq = &pctl->gpio_chip.irq;
        unsigned *pparents = devm_kcalloc(pctl->dev, 1, sizeof(*girq->parents), GFP_KERNEL);
        if (!pparents)
            return -ENOMEM;
        pparents[0] = pctl->irqs[0];
        girq->chip = &pctl->irq_chip;
        girq->parent_handler = apple_s5l8940x_gpio_gpio_irq_handler;
        girq->num_parents = 1;
        girq->parents = pparents;
        girq->default_type = IRQ_TYPE_NONE;
        girq->handler = handle_level_irq;
    }

    ret = gpiochip_add_data(&pctl->gpio_chip, pctl);
    if(ret < 0) {
        dev_err(pctl->dev, "Failed to add GPIO chip (%d).\n", ret);
        return ret;
    }

    /*
     * This used to force "I2C0 pins 4 and 5" into the iic function.  Those
     * are not I2C0's pins.  The ADT writes a GPIO as (port << 8) | pin, eight
     * pins to a port: function-iic_sda is 0x0604 and function-iic_scl 0x0605,
     * which are pins 52 and 53 -- and iBoot has them configured already.  The
     * old code took the low byte and rewrote pins 4 and 5, which belong to
     * something else, and the touch driver then found that "reset on pin 5
     * clobbers SCL".  The same misreading put the digitizer's reset on 5 and
     * its chip select on 7; they are 0x0205 = 21 and 0x0c07 = 103.
     */

    ret = gpiochip_add_pin_range(&pctl->gpio_chip, dev_name(pctl->dev), 0, 0, pctl->npins);
    if(ret < 0) {
        dev_err(pctl->dev, "Failed to add GPIO range (%d).\n", ret);
        gpiochip_remove(&pctl->gpio_chip);
        return ret;
    }

    return 0;
}

static const struct of_device_id apple_s5l8940x_gpio_pinctrl_of_match[] = {
    { .compatible = "apple,s5l8940x-gpio", },
    { },
};

static int apple_s5l8940x_gpio_pinctrl_probe(struct platform_device *pdev)
{
    struct apple_s5l8940x_gpio_pinctrl *pctl;
    struct resource *rsrc;
    int res;
    unsigned i;

    pctl = devm_kzalloc(&pdev->dev, sizeof(*pctl), GFP_KERNEL);
    if(!pctl)
        return -ENOMEM;
    pctl->dev = &pdev->dev;
    dev_set_drvdata(&pdev->dev, pctl);

    res = platform_irq_count(pdev); /* may return EPROBE_DEFER */
    if(res < 0)
        return res;
    if(!res) {
        dev_err(&pdev->dev, "Hx GPIO must have at least one IRQ.\n");
        return -EINVAL;
    }
    pctl->nirqgrps = res;

    of_property_read_u32(pdev->dev.of_node, "pin-count", &pctl->npins);
    if(pctl->npins < 0) {
        dev_err(&pdev->dev, "Hx GPIO must have 'pin-count' property.\n");
        return -EINVAL;
    }

    of_property_read_u32(pdev->dev.of_node, "pin-base", &pctl->pin_base);
    if(of_property_read_string(pdev->dev.of_node, "pin-prefix", &pctl->pin_prefix))
        pctl->pin_prefix = "gpio";

    pctl->pins = devm_kzalloc(&pdev->dev, sizeof(pctl->pins[0]) * pctl->npins, GFP_KERNEL);
    if(!pctl->pins)
        return -ENOMEM;
    pctl->pin_names = devm_kzalloc(&pdev->dev, sizeof(pctl->pin_names[0]) * pctl->npins, GFP_KERNEL);
    if(!pctl->pin_names)
        return -ENOMEM;
    pctl->pin_nums = devm_kzalloc(&pdev->dev, sizeof(pctl->pin_nums[0]) * pctl->npins, GFP_KERNEL);
    if(!pctl->pin_nums)
        return -ENOMEM;
    pctl->pin_cfgs = devm_kzalloc(&pdev->dev, sizeof(pctl->pin_cfgs[0]) * pctl->npins, GFP_KERNEL);
    if(!pctl->pin_cfgs)
        return -ENOMEM;
    pctl->irqs = devm_kzalloc(&pdev->dev, sizeof(pctl->irqs[0]) * pctl->nirqgrps, GFP_KERNEL);
    if(!pctl->pins)
        return -ENOMEM;

    for(i=0; i<pctl->nirqgrps; i++) {
        res = platform_get_irq(pdev, i);
        if(res < 0) {
            if(res != -EPROBE_DEFER)
                dev_err(&pdev->dev, "Failed to map IRQ %d (%d).\n", i, res);
            return res;
        }
        pctl->irqs[i] = res;
    }

    rsrc = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    pctl->base = devm_ioremap_resource(&pdev->dev, rsrc);
    if(IS_ERR(pctl->base))
        return PTR_ERR(pctl->base);

    for(i=0; i<pctl->npins; i++) {
        apple_s5l8940x_gpio_init_reg(pctl, i);

        pctl->pins[i].number = pctl->pin_base + i;
        pctl->pins[i].name = devm_kasprintf(&pdev->dev, GFP_KERNEL, "%s%d", pctl->pin_prefix, i);
        pctl->pins[i].drv_data = pctl;
        pctl->pin_names[i] = pctl->pins[i].name;
        pctl->pin_nums[i] = i;
    }

    pctl->pinctrl_desc.name = pctl->pin_prefix;
    pctl->pinctrl_desc.pins = pctl->pins;
    pctl->pinctrl_desc.npins = pctl->npins;
    pctl->pinctrl_desc.pctlops = &apple_s5l8940x_gpio_pinctrl_ops;
    pctl->pinctrl_desc.pmxops = &apple_s5l8940x_gpio_pinmux_ops;
    pctl->pinctrl_desc.confops = &apple_s5l8940x_gpio_pinconf_ops;

    pctl->pctldev = devm_pinctrl_register(&pdev->dev, &pctl->pinctrl_desc, pctl);
    if (IS_ERR(pctl->pctldev)) {
        dev_err(&pdev->dev, "Failed to register pinctrl device.\n");
        return PTR_ERR(pctl->pctldev);
    }

    writel(0, pctl->base + REG_LOCK);

    /* XNU's start, before anything can be let through: every pin's interrupt
     * off and every status bit cleared, so whatever iBoot left armed cannot
     * hold AIC 119 up the moment the chained handler opens it.  Then the NPL
     * mode XNU always sets on this device (the ADT has no "no-npl-mode");
     * what it does is not known, so the old value goes to the log. */
    for(i = 0; i < IRQ_NWORDS; i++) {
        writel(~0u, pctl->base + REG_IRQ_DISABLE(i * 32));
        writel(~0u, pctl->base + REG_IRQ_STATUS(i * 32));
    }
    dev_info(&pdev->dev, "interrupts quiesced as XNU does; NPL mode %#x -> 1\n",
             readl(pctl->base + REG_NPL_MODE));
    writel(1, pctl->base + REG_NPL_MODE);

    return apple_s5l8940x_gpio_gpio_register(pctl);
}

static struct platform_driver apple_s5l8940x_gpio_pinctrl_driver = {
    .driver = {
        .name = "apple-s5l8940x-gpio-pinctrl",
        .of_match_table = apple_s5l8940x_gpio_pinctrl_of_match,
        .suppress_bind_attrs = true,
    },
    .probe = apple_s5l8940x_gpio_pinctrl_probe,
};

static int __init apple_s5l8940x_gpio_pinctrl_register(void)
{
    return platform_driver_register(&apple_s5l8940x_gpio_pinctrl_driver);
}
arch_initcall(apple_s5l8940x_gpio_pinctrl_register);
