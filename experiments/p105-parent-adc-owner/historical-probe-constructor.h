// SPDX-License-Identifier: GPL-2.0-only
/* Exact historical inline allocations/return checks after P105 match. */
static int historical_parent_init(struct apple_pmu_i2c *pmu, struct i2c_client *i2c)
{
 int ret;
        pmu->bank0 = devm_kzalloc(&i2c->dev, sizeof(*pmu->bank0), GFP_KERNEL);
        if (!pmu->bank0)
            return -ENOMEM;
        ret = d1946_bank0_init(pmu->bank0, i2c);
        if (ret)
            return ret;
        d1946_bank0_config(pmu->bank0, &pmu->config);
        pmu->regmap = devm_regmap_init(&i2c->dev, d1946_bank0_regmap_bus(),
                                       pmu->bank0, &pmu->config);
    if(IS_ERR(pmu->regmap)) {
        ret = PTR_ERR(pmu->regmap);
        dev_err(&i2c->dev, "failed to create regmap: %d.\n", ret);
        return ret;
    }

return 0;
}
