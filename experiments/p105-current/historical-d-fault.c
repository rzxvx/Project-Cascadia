// SPDX-License-Identifier: GPL-2.0
/* Frozen v3 original, negative test control only. Not part of the module. */
static void d_fault(struct d1946_current *d)
{
	d->fault_latched = true;
	d->applied_valid = false;
	d_invalidate(d);
}
