// SPDX-License-Identifier: GPL-2.0
/* Actual pre-fix d_set, source SHA256 eae946c7491c2e70412ff383e62c6efa4e86d6665025155b0a09fe3965444a55. */
static int d_set(struct power_supply *psy, enum power_supply_property property,
		 const union power_supply_propval *v)
{
	struct d1946_current *d = power_supply_get_drvdata(psy);
	unsigned int ma, i;
	int ret = 0;
	if (v->intval < 0 || v->intval % 1000)
		return -EINVAL;
	ma = v->intval / 1000;
	if ((property == POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT &&
	     d1946_input_profile(ma) < 0) ||
	    (property == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT &&
	     d1946_charge_request(ma)))
		return -EINVAL;
	mutex_lock(&d->lifecycle_lock);
	mutex_lock(&d->io_lock);
	if (d->stopping || (ma && d->fault_latched))
		ret = -EIO;
	else if (!READ_ONCE(d->ready))
		ret = -EAGAIN;
	else if (property == POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT)
		d->requested_input = ma;
	else if (property == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT)
		d->requested_charge = ma;
	else
		ret = -EINVAL;
	atomic_inc(&d->epoch);
	mutex_unlock(&d->io_lock);
	if (ret) {
		mutex_unlock(&d->lifecycle_lock);
		return ret;
	}
	/* Consumers/parent start disabled. Enabling is an explicit nonzero request. */
	mutex_lock(&d->bus_lock);
	if (ma && !d->events_enabled) {
		d->events_enabled = true;
		mutex_unlock(&d->bus_lock);
		for (i = 0; i < ARRAY_SIZE(source_events); i++)
			enable_irq(d->virq[i]);
		if (!READ_ONCE(d->fault_latched)) {
			d->parent_enabled = true;
			enable_irq(d->parent_irq);
		}
	} else
		mutex_unlock(&d->bus_lock);
	d_invalidate(d);
	ret = READ_ONCE(d->fault_latched) ? -EIO : 0;
	mutex_unlock(&d->lifecycle_lock);
	return ret;
}
