// SPDX-License-Identifier: GPL-2.0-only
/* Actual private system callback bodies. */
static int apple_pmu_d1946_power_off_prepare(struct sys_off_data *data)
{
    struct regmap *map = ((struct apple_pmu_i2c *)data->cb_data)->regmap;
    unsigned int i, v;
#if IS_ENABLED(CONFIG_APPLE_PMU_D1946_BANK0_EXPERIMENT)
    struct apple_pmu_i2c *pmu = data->cb_data;
    int ret;

    if (pmu->bank0) {
#if IS_ENABLED(CONFIG_APPLE_PMU_D1946_ADC6_OWNER_EXPERIMENT)
        if (pmu->adc6)
            p105_owned_adc6_close(pmu->adc6);
#endif
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
static int __maybe_unused apple_pmu_p105_suspend(struct device *dev)
{
	struct apple_pmu_i2c *pmu = dev_get_drvdata(dev);

	if (pmu->adc6)
		p105_owned_adc6_close(pmu->adc6);
	/* No resume callback recreates a physical quiescence witness. */
	return 0;
}
