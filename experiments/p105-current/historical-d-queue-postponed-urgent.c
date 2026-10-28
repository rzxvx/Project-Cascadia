// SPDX-License-Identifier: GPL-2.0
/* Exact earlier production queue helper; test-only scheduling counterexample. */
static void d_queue(struct d1946_current *d, unsigned long delay,
		    bool allow_fault)
{
	unsigned long flags;
	spin_lock_irqsave(&d->queue_lock, flags);
	if (READ_ONCE(d->ready) && READ_ONCE(d->events_enabled) &&
	    !READ_ONCE(d->stopping) &&
	    (allow_fault || !READ_ONCE(d->fault_latched)))
		mod_delayed_work(system_wq, &d->work, delay);
	spin_unlock_irqrestore(&d->queue_lock, flags);
}
