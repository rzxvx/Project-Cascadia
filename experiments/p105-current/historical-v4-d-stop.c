static int d_stop(struct d1946_current *d)
{
	unsigned long flags;
	int ret, input_ret;

	mutex_lock(&d->lifecycle_lock);
	if (READ_ONCE(d->stopping)) {
		ret = d->stop_error;
		mutex_unlock(&d->lifecycle_lock);
		return ret;
	}
	spin_lock_irqsave(&d->queue_lock, flags);
	WRITE_ONCE(d->stopping, true);
	WRITE_ONCE(d->ready, false);
	atomic_inc(&d->epoch);
	spin_unlock_irqrestore(&d->queue_lock, flags);
	if (d->parent_enabled && !READ_ONCE(d->parent_failed))
		disable_irq(d->parent_irq);
	/* parent_failed may have used disable_irq_nosync() in its own thread.
  * Drain even that already-disabled path before children/domain/handles free. */
	if (d->parent_irq >= 0)
		synchronize_irq(d->parent_irq);
	cancel_delayed_work_sync(&d->work);
	mutex_lock(&d->io_lock);
	d->applied_valid = false;
	d->status_valid = false;
	/* One attempt per control path; any error prevents a successful suspend. */
	ret = 0;
	if (d->events_enabled) {
		ret = d1946_current_limit(&d->io, 0, d->ceiling);
		if (ret)
			dev_err(d->dev,
				"shutdown inhibit failed; physical state unknown\n");
		input_ret = d1946_input_limit(&d->io, 0);
		if (input_ret)
			dev_err(d->dev,
				"shutdown zero-input failed; physical state unknown\n");
		if (!ret)
			ret = input_ret;
	}
	d->stop_error = ret;
	if (ret)
		d->fault_latched = true;
	mutex_unlock(&d->io_lock);
	mutex_unlock(&d->lifecycle_lock);
	return ret;
}
