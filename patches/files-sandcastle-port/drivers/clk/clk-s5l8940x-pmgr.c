/*
 * Copyright (C) 2020 Corellium LLC
 *
 * Clock gating via S5L8940X (Apple A5) SoC PMGR
 */

#include <linux/clk-provider.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/delay.h>
#include <asm/io.h>

#define PMGR_GATE       1
#define PMGR_TOUCH_CLK  2

#define MAX_BASES       4

struct clk_hx_pmgr {
    struct clk_hw hw;
    unsigned type;
    const char *name;
    void __iomem *bases[MAX_BASES];
    const unsigned *seq[4];
    unsigned seqn[4];
    u32 freq_target;
};

#define to_clk_hx_pmgr(_hw) container_of(_hw, struct clk_hx_pmgr, hw)

static void clk_hx_pmgr_run_seq(struct clk_hx_pmgr *clk, const unsigned *seq, unsigned n)
{
    if(!n || !seq)
        return;
    while(n --) {
        writel(seq[2], clk->bases[seq[0]] + seq[1]);
        seq += 3;
    }
}

/********************* PMGR_GATE *********************/

static int clk_hx_pmgr_gate_enable(struct clk_hw *hw)
{
    struct clk_hx_pmgr *clk = to_clk_hx_pmgr(hw);
    unsigned max = 10000;
    uint32_t val;

    clk_hx_pmgr_run_seq(clk, clk->seq[1], clk->seqn[1]);

    val = readl(clk->bases[0]);

    val |= 15;
    writel(val, clk->bases[0]);

    while(max --) {
        val = readl(clk->bases[0]);

        if(((val >> 4) & 15) == 15) {
            writel(val | 0x10000000, clk->bases[0]);
            clk_hx_pmgr_run_seq(clk, clk->seq[3], clk->seqn[3]);
            return 0;
        }

        cpu_relax();
    }

    pr_err("%s: failed to enable PMGR clock\n", clk->name);

    return -ETIMEDOUT;
}

static void clk_hx_pmgr_gate_disable(struct clk_hw *hw)
{
    struct clk_hx_pmgr *clk = to_clk_hx_pmgr(hw);
    unsigned max = 10000;
    uint32_t val;

    clk_hx_pmgr_run_seq(clk, clk->seq[0], clk->seqn[0]);

    val = readl(clk->bases[0]);
    val |= 0x300;
    val &= ~15;
    writel(val, clk->bases[0]);

    while(max --) {
        val = readl(clk->bases[0]);

        if(((val >> 4) & 15) == 0) {
            clk_hx_pmgr_run_seq(clk, clk->seq[2], clk->seqn[2]);
            return;
        }

        cpu_relax();
    }

    pr_err("%s: failed to disable PMGR clock\n", clk->name);
}

static int clk_hx_pmgr_gate_is_enabled(struct clk_hw *hw)
{
    struct clk_hx_pmgr *clk = to_clk_hx_pmgr(hw);
    uint32_t val;

    val = readl(clk->bases[0]);
    return ((val >> 4) & 15) == 15;
}

const struct clk_ops clk_hx_pmgr_gate_ops = {
    .enable = clk_hx_pmgr_gate_enable,
    .disable = clk_hx_pmgr_gate_disable,
    .is_enabled = clk_hx_pmgr_gate_is_enabled,
};

static int clk_prepare_hx_pmgr_gate(struct clk_hx_pmgr *clk_hx_pmgr, struct clk_init_data *init, struct device *dev, struct device_node *node, const char * const *parent_names, u8 num_parents, void __iomem **bases)
{
    init->ops = &clk_hx_pmgr_gate_ops;
    init->flags = CLK_SET_RATE_PARENT;
    return 0;
}

/********************* PMGR_TOUCH_CLK *********************/

#define REG_TOUCH_CLK_DISABLE   (1u << 31)
#define REG_TOUCH_CLK_ENABLE    (1u << 19)
#define REG_TOUCH_CLK_BUSY      (1u << 18)
#define REG_TOUCH_CLK_DIV_MASK  (1023u)

static int clk_hx_pmgr_touch_clk_enable(struct clk_hw *hw)
{
    struct clk_hx_pmgr *clk = to_clk_hx_pmgr(hw);
    unsigned max = 5000;
    uint32_t val, divider;
    unsigned long parent_freq;

    parent_freq = clk_hw_get_rate(clk_hw_get_parent(hw));
    divider = (parent_freq + clk->freq_target / 2) / clk->freq_target;

    if(divider < 1)
        divider = 1;
    if(divider > 1023) {
        pr_err("%s: divider of %d exceeds maximum.\n", clk->name, divider);
        return -EINVAL;
    }

    clk_hx_pmgr_run_seq(clk, clk->seq[1], clk->seqn[1]);

    val = readl(clk->bases[0]);

    val &= ~REG_TOUCH_CLK_DISABLE;
    writel(val, clk->bases[0]);

    val &= ~REG_TOUCH_CLK_DIV_MASK;
    val |= REG_TOUCH_CLK_ENABLE | (divider & REG_TOUCH_CLK_DIV_MASK);
    writel(val, clk->bases[0]);

    while(max --) {
        val = readl(clk->bases[0]);

        if(!(val & REG_TOUCH_CLK_BUSY)) {
            clk_hx_pmgr_run_seq(clk, clk->seq[3], clk->seqn[3]);
            return 0;
        }

        udelay(10);
    }

    pr_err("%s: failed to enable touch clock\n", clk->name);

    return -ETIMEDOUT;
}

static void clk_hx_pmgr_touch_clk_disable(struct clk_hw *hw)
{
    struct clk_hx_pmgr *clk = to_clk_hx_pmgr(hw);
    unsigned max = 5000;
    uint32_t val;

    clk_hx_pmgr_run_seq(clk, clk->seq[0], clk->seqn[0]);

    val = readl(clk->bases[0]);

    val &= ~REG_TOUCH_CLK_ENABLE;
    writel(val, clk->bases[0]);

    while(max --) {
        val = readl(clk->bases[0]);

        if(!(val & REG_TOUCH_CLK_BUSY))
            break;

        udelay(10);
    }

    if(!max)
        pr_err("%s: failed to disable touch clock\n", clk->name);

    val |= REG_TOUCH_CLK_DISABLE;
    writel(val, clk->bases[0]);

    udelay(100);

    clk_hx_pmgr_run_seq(clk, clk->seq[2], clk->seqn[2]);
}

static int clk_hx_pmgr_touch_clk_is_enabled(struct clk_hw *hw)
{
    struct clk_hx_pmgr *clk = to_clk_hx_pmgr(hw);
    uint32_t val;

    val = readl(clk->bases[0]);
    return !!(val & REG_TOUCH_CLK_ENABLE);
}

static unsigned long clk_hx_pmgr_touch_clk_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
    struct clk_hx_pmgr *clk = to_clk_hx_pmgr(hw);
    uint32_t val;

    val = readl(clk->bases[0]);
    val &= REG_TOUCH_CLK_DIV_MASK;
    if(!val)
        val = REG_TOUCH_CLK_DIV_MASK + 1;

    return parent_rate / val;
}

const struct clk_ops clk_hx_pmgr_touch_clk_ops = {
    .enable = clk_hx_pmgr_touch_clk_enable,
    .disable = clk_hx_pmgr_touch_clk_disable,
    .is_enabled = clk_hx_pmgr_touch_clk_is_enabled,
    .recalc_rate = clk_hx_pmgr_touch_clk_recalc_rate,
};

static int clk_prepare_hx_pmgr_touch_clk(struct clk_hx_pmgr *clk_hx_pmgr, struct clk_init_data *init, struct device *dev, struct device_node *node, const char * const *parent_names, u8 num_parents, void __iomem **bases)
{
    int err;

    if(num_parents != 1)
        return -ENODEV;

    err = of_property_read_u32_index(node, "clock-frequency", 0, &clk_hx_pmgr->freq_target);
    if(err) {
        dev_err(dev, "this clock requires a 'clock-frequency' setting.\n");
        return err;
    }

    init->ops = &clk_hx_pmgr_touch_clk_ops;
    return 0;
}

/********************* shared code *********************/

static int clk_hx_pmgr_driver_probe(struct platform_device *pdev)
{
    struct device_node *node = pdev->dev.of_node;
    const char **parent_names = NULL;
    unsigned num_parents, type;
    struct clk *clk;
    void __iomem *bases[MAX_BASES] = { NULL };
    int i, n, err;
    unsigned seqn[4] = { 0 }, *seq[4] = { NULL };
    static const char * const seqname[4] = { "pre-down", "pre-up", "post-down", "post-up" };
    struct clk_hx_pmgr *clk_hx_pmgr;
    struct clk_init_data init = {};
    struct clk_hw *hw;

    num_parents = of_clk_get_parent_count(node);
    if(num_parents) {
        parent_names = devm_kcalloc(&pdev->dev, num_parents, sizeof(char *), GFP_KERNEL);
        if(!parent_names)
            return -ENOMEM;
        of_clk_parent_fill(node, parent_names, num_parents);
    }

    n = of_property_count_elems_of_size(node, "reg", sizeof(uint32_t) * (of_n_addr_cells(node) + of_n_size_cells(node)));
    if(n < 0) {
        pr_err("%pOFn: %s: not enough MMIO ranges.\n", node, __func__);
        return -EINVAL;
    }
    if(n > MAX_BASES) {
        pr_err("%pOFn: %s: too many MMIO ranges.\n", node, __func__);
        return -EINVAL;
    }

    for(i=0; i<n; i++) {
        bases[i] = of_iomap(node, i);
        if(!bases[i]) {
            pr_err("%pOFn: %s: unable to map MMIO range %d.\n", node, __func__, i);
            return -EINVAL;
        }
    }

    for(i=0; i<4; i++) {
        n = of_property_count_elems_of_size(node, seqname[i], sizeof(uint32_t));
        if(n > 0) {
            seq[i] = devm_kcalloc(&pdev->dev, n, sizeof(unsigned), GFP_KERNEL);
            if(!seq[i])
                return -ENOMEM;
            seqn[i] = of_property_read_variable_u32_array(node, seqname[i], seq[i], 0, n) / 3;
        }
    }

    type = 0;
    if(of_device_is_compatible(node, "apple,s5l8940x-pmgr-clk-gate"))
        type = PMGR_GATE;
    if(of_device_is_compatible(node, "apple,s5l8940x-pmgr-clk-touch"))
        type = PMGR_TOUCH_CLK;

    clk_hx_pmgr = devm_kzalloc(&pdev->dev, sizeof(*clk_hx_pmgr), GFP_KERNEL);
    if(!clk_hx_pmgr)
        return -ENOMEM;

    clk_hx_pmgr->type = type;
    for(i=0; i<MAX_BASES; i++)
        clk_hx_pmgr->bases[i] = bases[i];
    for(i=0; i<4; i++) {
        clk_hx_pmgr->seq[i] = seq[i];
        clk_hx_pmgr->seqn[i] = seqn[i];
    }
    clk_hx_pmgr->name = node->name;

    init.name = node->name;
    init.parent_names = parent_names;
    init.num_parents = num_parents;

    switch(type) {
    case PMGR_GATE:
        err = clk_prepare_hx_pmgr_gate(clk_hx_pmgr, &init, &pdev->dev, node, parent_names, num_parents, bases);
        break;
    case PMGR_TOUCH_CLK:
        err = clk_prepare_hx_pmgr_touch_clk(clk_hx_pmgr, &init, &pdev->dev, node, parent_names, num_parents, bases);
        break;
    default:
        pr_err("%pOFn: %s: unsupported device type\n", node, __func__);
        return -EINVAL;
    }
    if(err)
        return err;

    clk_hx_pmgr->hw.init = &init;

    hw = &clk_hx_pmgr->hw;
    err = devm_clk_hw_register(&pdev->dev, hw);
    if(err)
        return err;

    clk = hw->clk;
    return of_clk_add_provider(node, of_clk_src_simple_get, clk);
}

static const struct of_device_id clk_hx_pmgr_match_table[] = {
    { .compatible = "apple,s5l8940x-pmgr-clk-gate" },
    { .compatible = "apple,s5l8940x-pmgr-clk-touch" },
    { }
};

static struct platform_driver clk_hx_pmgr_driver = {
    .probe = clk_hx_pmgr_driver_probe,
    .driver = {
        .name = "clk-s5l8940x-pmgr",
        .of_match_table = clk_hx_pmgr_match_table,
    },
};
builtin_platform_driver(clk_hx_pmgr_driver);
