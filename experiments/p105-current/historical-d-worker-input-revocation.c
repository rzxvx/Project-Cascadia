// SPDX-License-Identifier: GPL-2.0
/* Exact prior worker; no pre-positive-input revalidation. Test-only GPL fixture. */
static void d_worker(struct work_struct *work)
{
	struct d1946_current *d =
		container_of(to_delayed_work(work), struct d1946_current, work);
	struct d1946_snapshot snapshot = {};
	int ret = 0;
	unsigned int input = 0, charge = 0;
	unsigned int epoch = atomic_read(&d->epoch);
	bool usable = false;
	if (!READ_ONCE(d->ready) || !READ_ONCE(d->events_enabled) ||
	    READ_ONCE(d->stopping))
		return;
	/* Supplier must implement actual UDC/accessory permission; no source guessing. */
	ret = d->provider->snapshot(d->provider->context, d->input, d->battery,
				    &snapshot);
	if (!ret && !d_snapshot_valid(d, &snapshot))
		ret = -ESTALE;
	mutex_lock(&d->io_lock);
	if (ret)
		d_fault(d);
	if (d->fault_latched)
		goto fault;
	ret = d_read(d, 0x0a, d->status, sizeof(d->status));
	if (ret) {
		d_fault(d);
		goto fault;
	}
	d->status_valid = true;
	d->status_epoch = epoch;
	d->status_time_ns = ktime_get_ns();
	/* END/TO/HighOrLowTemperature are observed, not invented recovery recipes. */
	if (d->status[1] & 0x38) {
		d_fault(d);
		goto fault;
	}
	/* 2.8..4.1V is a conservative experimental stop window, NOT a CV
		* target or a physically calibrated protection guarantee. */
	usable = snapshot.online == 1 && snapshot.budget_ua > 0 &&
		 snapshot.present == 1 && snapshot.voltage_uv >= 2800000 &&
		 snapshot.voltage_uv <= 4100000 && snapshot.temp_decic >= 100 &&
		 snapshot.temp_decic <= 450 && (d->status[0] & 8);
	if (usable) {
		unsigned int maximum = min_t(unsigned int,
					     snapshot.budget_ua / 1000,
					     d->requested_input);
		input = d_profile_floor(min_t(unsigned int, maximum, 2100));
		charge = d_charge_for_input(d, input, snapshot.budget_ua);
	}
	/* A validator can return its previous result after a concurrent source
	 * callback advances the frontend epoch. Recheck epoch after each grant
	 * validation below; this does not make bus I/O atomic with revocation. */
	/* A fresh unchanged plan needs only status/permission validation. Avoid
	 * repeatedly toggling the charger on every 100ms telemetry poll. */
	if (d->applied_valid && d->applied_epoch == epoch &&
	    d->applied_input == d_decoded_input(input) &&
	    d->applied_charge ==
		    min(charge / 40, (unsigned int)d->ceiling) * 40 &&
	    epoch == atomic_read(&d->epoch) && d_snapshot_valid(d, &snapshot) &&
	    epoch == atomic_read(&d->epoch))
		goto publish;
	/* Inhibit charge while changing input policy; never enable from stale epoch. */
	d->applied_valid = false;
	ret = d1946_current_limit(&d->io, 0, d->ceiling);
	if (!ret)
		ret = d1946_input_limit(&d->io, input);
	if (ret) {
		d_fault(d);
		goto fault;
	}
	if (epoch != atomic_read(&d->epoch) ||
	    !d_snapshot_valid(d, &snapshot) ||
	    epoch != atomic_read(&d->epoch) || READ_ONCE(d->stopping)) {
		ret = -EAGAIN;
		goto out;
	}
	if (charge)
		ret = d1946_current_limit(&d->io, charge, d->ceiling);
	if (ret) {
		d_fault(d);
		goto fault;
	}
	/* A source event can arrive during the final bus write. Inhibit again
		* on invalidation; this is not a proof of zero transient charge current. */
	if (epoch != atomic_read(&d->epoch) ||
	    !d_snapshot_valid(d, &snapshot) ||
	    epoch != atomic_read(&d->epoch) || READ_ONCE(d->stopping)) {
		ret = d1946_current_limit(&d->io, 0, d->ceiling);
		if (ret) {
			d_fault(d);
			goto fault;
		}
		ret = -EAGAIN;
		goto out;
	}
publish:
	d->applied_input = d_decoded_input(input);
	d->applied_charge = min(charge / 40, (unsigned int)d->ceiling) * 40;
	d->applied_epoch = epoch;
	d->applied_valid = true;
	goto out;
fault:
	if (!d->inhibit_attempted) {
		d->inhibit_attempted = true;
		ret = d1946_current_limit(&d->io, 0, d->ceiling);
		if (ret)
			dev_err(d->dev,
				"inhibit failed; physical charging state unknown: %d\n",
				ret);
		ret = d1946_input_limit(&d->io, 0);
		if (ret)
			dev_err(d->dev,
				"zero-input failed; physical input state unknown: %d\n",
				ret);
	}
out:
	mutex_unlock(&d->io_lock);
	if (d->psy)
		power_supply_changed(d->psy);
	d_queue(d, ret == -EAGAIN ? 0 : msecs_to_jiffies(100), false);
}
