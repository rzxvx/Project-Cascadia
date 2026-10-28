// SPDX-License-Identifier: GPL-2.0-only
/* Actual scoped MFD constructor; unchanged from private source. */
static int apple_pmu_d1946_owned_parent_init(struct apple_pmu_i2c *pmu,
					   struct i2c_client *client)
{
	int ret;

	pmu->bank0 = devm_kzalloc(&client->dev, sizeof(*pmu->bank0), GFP_KERNEL);
	if (!pmu->bank0)
		return -ENOMEM;
	ret = d1946_bank0_init(pmu->bank0, client);
	if (ret)
		return ret;
	/* Immediate rollback for failures before the map/late-stop exists. */
	ret = devm_add_action_or_reset(&client->dev, d1946_bank0_stop, pmu->bank0);
	if (ret)
		return ret;
	d1946_bank0_config(pmu->bank0, &pmu->config);
	pmu->regmap = devm_regmap_init(&client->dev, d1946_bank0_regmap_bus(),
				     pmu->bank0, &pmu->config);
	if (IS_ERR(pmu->regmap))
		return PTR_ERR(pmu->regmap);
	/* Before children/sys-off, after map: drain before map destruction.
	 * The earlier rollback action is idempotent and never releases twice. */
	ret = devm_add_action_or_reset(&client->dev, d1946_bank0_stop, pmu->bank0);
	if (ret)
		return ret;
#if IS_ENABLED(CONFIG_APPLE_PMU_D1946_ADC6_OWNER_EXPERIMENT)
	if (p105_adc6_owner) {
		pmu->adc6 = p105_owned_adc6_create(&client->dev, pmu->bank0);
		if (IS_ERR(pmu->adc6))
			return PTR_ERR(pmu->adc6);
	}
#endif
	return 0;
}
