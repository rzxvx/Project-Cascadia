// SPDX-License-Identifier: GPL-2.0
/* Exact earlier GPL frontend d_probe at SHA426c5d8d...; unbanked parent gap.
 * Not built into runtime; tests execute only its prefix before allocation. */
static int d_probe(struct platform_device *pdev)
{
	struct apple_pmu_i2c *pmu;
	struct d1946_current *d;
	struct power_supply_config cfg = {};
	unsigned int i;
	u8 control;
	int ret;
	/* No default probe-side PMU/GPIO operations or automatic read-clear scan. */
	if (!allow_experiment || !retained_analog_confirmed ||
	    !of_property_read_bool(pdev->dev.of_node,
				   "apple,p105-current-only-experiment") ||
	    !of_property_read_bool(pdev->dev.of_node,
				   "apple,no-input-calibration"))
		return -EPERM;
	pmu = dev_get_drvdata(pdev->dev.parent);
	if (!pmu || !pmu->regmap)
		return -EPROBE_DEFER;
	d = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->dev = &pdev->dev;
	d->map = pmu->regmap;
	d->parent_irq = -1;
	mutex_init(&d->bus_lock);
	mutex_init(&d->io_lock);
	mutex_init(&d->lifecycle_lock);
	spin_lock_init(&d->queue_lock);
	atomic_set(&d->epoch, 0);
	INIT_DELAYED_WORK(&d->work, d_worker);
	d->io = (struct d1946_io){ d, d_read, d_write };
	ret = d_providers_acquire(d);
	if (ret)
		goto fail;
	ret = d_read(d, 0x12, d->boot_masks, D_NBANKS);
	if (ret)
		goto fail;
	memcpy(d->masks, d->boot_masks, D_NBANKS);
	memcpy(d->desired, d->boot_masks, D_NBANKS);
	ret = d_read(d, 0x3f, &control, 1);
	if (ret)
		goto fail;
	d->ceiling = control & 63;
	d->chip = (struct irq_chip){ .name = "d1946-raw-events",
				     .irq_mask = d_irq_mask,
				     .irq_unmask = d_irq_unmask,
				     .irq_bus_lock = d_irq_bus_lock,
				     .irq_bus_sync_unlock = d_irq_bus_sync };
	d->domain = irq_domain_add_linear(pdev->dev.of_node, D_NEVENTS,
					  &d_domain_ops, d);
	if (!d->domain) {
		ret = -ENOMEM;
		goto fail;
	}
	d->parent_irq = platform_get_irq(pdev, 0);
	if (d->parent_irq < 0) {
		ret = d->parent_irq;
		d->parent_irq = -1;
		goto fail;
	}
	if (irq_get_trigger_type(d->parent_irq) != IRQ_TYPE_LEVEL_LOW) {
		ret = -EINVAL;
		d->parent_irq = -1;
		goto fail;
	}
	ret = request_threaded_irq(d->parent_irq, NULL, d_parent,
				   IRQF_ONESHOT | IRQF_NO_AUTOEN,
				   dev_name(d->dev), d);
	if (ret) {
		d->parent_irq = -1;
		goto fail;
	}
	for (i = 0; i < ARRAY_SIZE(source_events); i++) {
		d->virq[i] = irq_create_mapping(d->domain, source_events[i]);
		if (!d->virq[i]) {
			ret = -ENOMEM;
			goto fail;
		}
		ret = request_threaded_irq(d->virq[i], NULL, d_event,
					   IRQF_ONESHOT | IRQF_NO_AUTOEN,
					   dev_name(d->dev), d);
		if (ret) {
			irq_dispose_mapping(d->virq[i]);
			d->virq[i] = 0;
			goto fail;
		}
	}
	d_prepare_desc(d);
	cfg.drv_data = d;
	cfg.of_node = pdev->dev.of_node;
	d->psy = power_supply_register(d->dev, &d->desc, &cfg);
	if (IS_ERR(d->psy)) {
		ret = PTR_ERR(d->psy);
		d->psy = NULL;
		goto fail;
	}
	platform_set_drvdata(pdev, d);
	WRITE_ONCE(d->ready, true);
	dev_warn(
		d->dev,
		"UNVALIDATED current-only experiment; no CV/termination/failsafe initialization\n");
	return 0;
fail:
	d_release(d);
	return ret;
}
