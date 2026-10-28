// SPDX-License-Identifier: GPL-2.0
/* Private, unvalidated P105 inherited-analog-state experiment.
 * No DT node is supplied; Kconfig and experiment parameters default off.
 * No CV, termination, OTP, watchdog or analog-fault initialization is invented.
 */
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/power_supply.h>
#include <linux/apple-pmu-i2c.h>
#include <linux/ktime.h>
#include <linux/jiffies.h>
#include <linux/atomic.h>
#include <linux/of_platform.h>
#include <linux/spinlock.h>
#include <linux/pm.h>
#include "d1946-current-provider.h"
#include "d1946-current-core.h"
#include "../p105-source/p105-combined-provider.h"

static bool allow_experiment;
module_param(allow_experiment, bool, 0444);
MODULE_PARM_DESC(allow_experiment,
		 "Explicitly allow the unvalidated current-only experiment");
static bool retained_analog_confirmed;
module_param(retained_analog_confirmed, bool, 0444);
MODULE_PARM_DESC(
	retained_analog_confirmed,
	"Operator assertion of independently verified analog retention; not a hardware test");

/* Explicit opt-in to the separately reviewed, real USB2/gauge producer.
 * Still gated by both inherited-state experiment assertions; no DT is supplied. */
static bool owned_usb2_source;
/* Further opt-in experiment only; no physical charging or analog protection
 * guarantee follows from software current/profile evidence. */
static bool allow_usb2_current;
module_param(allow_usb2_current, bool, 0444);
MODULE_PARM_DESC(
	allow_usb2_current,
	"Experimental owned USB2 current plan; requires every existing retention/source/gauge assertion");
module_param(owned_usb2_source, bool, 0444);
MODULE_PARM_DESC(
	owned_usb2_source,
	"Use the actual owned USB2/gauge provider instead of platform data");

#define D_NEVENTS 72
#define D_NBANKS 9
#define D_EXPERIMENT_MAX_MA 720
static const unsigned int source_events[] = { 3, 4, 52 };
struct d1946_current {
	struct device *dev;
	struct regmap *map;
	struct d1946_io io;
	struct mutex bus_lock, io_lock, lifecycle_lock;
	spinlock_t queue_lock;
	const struct d1946_current_provider *provider;
	struct p105_combined_provider *owned_provider;
	struct notifier_block owned_source_notifier;
	struct platform_device *source_pdev;
	struct irq_chip chip;
	struct irq_domain *domain;
	int parent_irq, virq[3], stop_error;
	unsigned int requested_input, requested_charge, applied_input,
		applied_charge;
	u8 boot_masks[D_NBANKS], masks[D_NBANKS], desired[D_NBANKS],
		dirty[D_NBANKS];
	u8 owned[D_NBANKS];
	u8 ceiling, status[6];
	bool events_enabled, stopping, fault_latched, inhibit_attempted;
	bool parent_failed, parent_enabled, ready, applied_valid, status_valid;
	atomic_t epoch;
	unsigned int applied_epoch, status_epoch;
	u64 status_time_ns;
	struct delayed_work work;
	struct power_supply *psy, *input, *battery;
	struct power_supply_desc desc;
};

static int d_read(void *context, unsigned int reg, u8 *data, unsigned int n)
{
	return regmap_bulk_read(((struct d1946_current *)context)->map, reg,
				data, n);
}
static int d_write(void *context, unsigned int reg, const u8 *data,
		   unsigned int n)
{
	return regmap_bulk_write(((struct d1946_current *)context)->map, reg,
				 data, n);
}
/* Scheduling is serialized with stop. A notifier/IRQ that observed old
 * ready state cannot queue work after cancel_delayed_work_sync() returns. */
static void d_queue(struct d1946_current *d, unsigned long delay,
		    bool allow_fault)
{
	unsigned long flags;
	spin_lock_irqsave(&d->queue_lock, flags);
	if (READ_ONCE(d->ready) && READ_ONCE(d->events_enabled) &&
	    !READ_ONCE(d->stopping) &&
	    (allow_fault || !READ_ONCE(d->fault_latched))) {
		/* A periodic self-requeue must not postpone an already pending
		 * source event. Urgent work may still advance a pending poll. */
		if (delay)
			queue_delayed_work(system_wq, &d->work, delay);
		else
			mod_delayed_work(system_wq, &d->work, 0);
	}
	spin_unlock_irqrestore(&d->queue_lock, flags);
}
static void d_invalidate(struct d1946_current *d)
{
	atomic_inc(&d->epoch);
	d_queue(d, 0, true);
}
#if IS_REACHABLE(CONFIG_P105_COMBINED_CURRENT_PROVIDER)
static int d_owned_source_changed(struct notifier_block *nb,
				  unsigned long reason, void *unused)
{
	struct d1946_current *d =
		container_of(nb, struct d1946_current, owned_source_notifier);
	(void)reason;
	(void)unused;
	d_invalidate(d);
	return NOTIFY_OK;
}
#endif
/* io_lock held. A fault never silently restores permission after a good read. */
static void d_fault(struct d1946_current *d)
{
	bool first = !d->fault_latched;
	d->fault_latched = true;
	d->applied_valid = false;
	/* Preserve invalidation for every failure, but never self-requeue an
  * already-latched worker after its bounded inhibit attempt. */
	atomic_inc(&d->epoch);
	if (first)
		d_queue(d, 0, true);
}

static void d_irq_bus_lock(struct irq_data *id)
{
	mutex_lock(&((struct d1946_current *)irq_data_get_irq_chip_data(id))
			    ->bus_lock);
}
static void d_irq_mask(struct irq_data *id)
{
	struct d1946_current *d = irq_data_get_irq_chip_data(id);
	unsigned int bank = id->hwirq / 8;
	u8 bit = 1u << (id->hwirq % 8);
	if (!d->events_enabled)
		return;
	d->desired[bank] |= bit;
	d->dirty[bank] |= bit;
	d->owned[bank] |= bit;
}
static void d_irq_unmask(struct irq_data *id)
{
	struct d1946_current *d = irq_data_get_irq_chip_data(id);
	unsigned int bank = id->hwirq / 8;
	u8 bit = 1u << (id->hwirq % 8);
	if (!d->events_enabled || READ_ONCE(d->fault_latched))
		return;
	d->desired[bank] &= ~bit;
	d->dirty[bank] |= bit;
	d->owned[bank] |= bit;
}
static void d_irq_bus_sync(struct irq_data *id)
{
	struct d1946_current *d = irq_data_get_irq_chip_data(id);
	unsigned int bank;
	int ret;
	mutex_lock(&d->io_lock);
	for (bank = 0; bank < D_NBANKS; bank++) {
		u8 actual, bits = d->dirty[bank];
		if (!bits)
			continue;
		ret = d1946_update_checked(&d->io, 0x12 + bank, bits,
					   d->desired[bank]);
		if (!ret)
			ret = d_read(d, 0x12 + bank, &actual, 1);
		if (ret) {
			d_fault(d);
			break;
		}
		d->masks[bank] = actual;
		d->desired[bank] = (d->desired[bank] & bits) | (actual & ~bits);
		d->dirty[bank] = 0;
	}
	mutex_unlock(&d->io_lock);
	mutex_unlock(&d->bus_lock);
}
static int d_domain_map(struct irq_domain *domain, unsigned int virq,
			irq_hw_number_t hwirq)
{
	struct d1946_current *d = domain->host_data;
	if (hwirq >= D_NEVENTS)
		return -EINVAL;
	irq_set_chip_data(virq, d);
	irq_set_chip_and_handler(virq, &d->chip, handle_simple_irq);
	irq_set_nested_thread(virq, 1);
	irq_set_noprobe(virq);
	return 0;
}
static const struct irq_domain_ops d_domain_ops = {
	.map = d_domain_map,
	.xlate = irq_domain_xlate_onecell,
};
static irqreturn_t d_event(int irq, void *context)
{
	d_invalidate(context);
	return IRQ_HANDLED;
}
static irqreturn_t d_parent(int irq, void *context)
{
	struct d1946_current *d = context;
	u8 event[D_NBANKS], masked[D_NBANKS];
	unsigned int bank, bit;
	int ret;
	if (READ_ONCE(d->stopping))
		return IRQ_HANDLED;
	mutex_lock(&d->io_lock);
	/* Only this thread consumes event bytes; no probe read/guessed ack write. */
	ret = d_read(d, 0x01, event, D_NBANKS);
	if (ret) {
		d->parent_failed = true;
		d_fault(d);
	} else {
		memcpy(masked, d->masks, sizeof(masked));
	}
	mutex_unlock(&d->io_lock);
	if (ret) {
		disable_irq_nosync(irq);
		return IRQ_HANDLED;
	}
	for (bank = 0; bank < D_NBANKS; bank++)
		for (bit = 0; bit < 8; bit++)
			if ((event[bank] & ~masked[bank]) & (1u << bit)) {
				unsigned int child = irq_find_mapping(
					d->domain, bank * 8 + bit);
				if (child)
					handle_nested_irq(child);
				/* Unclaimed events have no semantic permission; no reinterpretation. */
			}
	return IRQ_HANDLED;
}

static bool d_snapshot_valid(struct d1946_current *d,
			     const struct d1946_snapshot *s)
{
	u64 now = ktime_get_ns();
	return s->sampled_ns <= now && s->valid_until_ns > now &&
	       s->valid_until_ns - s->sampled_ns <= NSEC_PER_SEC &&
	       d->provider->valid(d->provider->context, s);
}
static unsigned int d_profile_floor(unsigned int maximum)
{
	static const unsigned int ma[] = { 2100, 1500, 1000, 500, 100 };
	unsigned int i;
	for (i = 0; i < ARRAY_SIZE(ma); i++)
		if (maximum >= ma[i])
			return ma[i];
	return 0;
}
static unsigned int d_decoded_input(unsigned int ma)
{
	switch (ma) {
	case 1000:
		return 975;
	case 1500:
		return 1450;
	case 2100:
		return 2050;
	default:
		return ma;
	}
}
static unsigned int d_charge_for_input(struct d1946_current *d,
				       unsigned int input,
				       unsigned int budget_ua)
{
	unsigned int charge;
	if (owned_usb2_source) {
		/* The actual owned producer only publishes completed USB2 <=500mA.
		 * No USB pre-configuration or accessory permission is inferred here. */
		if (!allow_usb2_current || !input || input > 500 ||
		    budget_ua > 500000)
			return 0;
		charge = min(min(d->requested_charge, D_EXPERIMENT_MAX_MA),
			     input);
	} else {
		/* Keep the existing legacy-provider experiment restriction. */
		if (input < 1000)
			return 0;
		charge = min(d->requested_charge, D_EXPERIMENT_MAX_MA);
	}
	/* Actual D1946 setters floor to 40mA codes. Keep both zero thermal caps;
	 * their physical temperature/source bucket identities remain unknown. */
	return charge / 40 * 40;
}
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
	/* A known PMU/IRQ fault needs only the bounded inhibit path. Do not
	 * delay it with an unrelated fresh supplier/gauge capture. Faults that
	 * arrive after this check remain handled by the later locked check. */
	mutex_lock(&d->io_lock);
	if (d->fault_latched)
		goto fault;
	mutex_unlock(&d->io_lock);
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
		 snapshot.temp_decic <= 400 && (d->status[0] & 8);
	if (usable) {
		unsigned int maximum = min_t(unsigned int,
					     snapshot.budget_ua / 1000,
					     d->requested_input);
		input = d_profile_floor(min_t(unsigned int, maximum, 2100));
		charge = d_charge_for_input(d, input, snapshot.budget_ua);
	}
	/* A checked ISET readback alone does not establish that hardware uses it.
	 * Check inherited input mode even for an unchanged positive plan, before
	 * the fast path can publish it again. Zero/fault inhibit needs no grant.
	 * Subsequent source/epoch checks still fence all positive programming.
	 */
	if (input) {
		ret = d1946_input_handoff(&d->io);
		if (ret) {
			d_fault(d);
			goto fault;
		}
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
	/* Inhibit even after revocation, but never start a new positive input
	 * plan from a snapshot whose epoch/lease is already known stale. The
	 * callback can revoke during valid(), so recheck epoch after it too. */
	if (!ret && input &&
	    (epoch != atomic_read(&d->epoch) ||
	     !d_snapshot_valid(d, &snapshot) ||
	     epoch != atomic_read(&d->epoch) || READ_ONCE(d->stopping))) {
		ret = -EAGAIN;
		goto out;
	}
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
static const enum power_supply_property d_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT,
};
static int d_get(struct power_supply *psy, enum power_supply_property property,
		 union power_supply_propval *v)
{
	struct d1946_current *d = power_supply_get_drvdata(psy);
	int ret = 0;
	mutex_lock(&d->io_lock);
	switch (property) {
	case POWER_SUPPLY_PROP_HEALTH:
		v->intval = d->fault_latched ?
				    POWER_SUPPLY_HEALTH_UNSPEC_FAILURE :
				    POWER_SUPPLY_HEALTH_UNKNOWN;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		v->intval = !d->status_valid || d->fault_latched ||
					    d->status_epoch !=
						    atomic_read(&d->epoch) ||
					    ktime_get_ns() - d->status_time_ns >
						    NSEC_PER_SEC ?
				    POWER_SUPPLY_STATUS_UNKNOWN :
			    (d->status[1] & 6) ?
				    POWER_SUPPLY_STATUS_CHARGING :
				    POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
	case POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT:
		if (!d->applied_valid ||
		    d->applied_epoch != atomic_read(&d->epoch))
			ret = d->fault_latched ? -EIO : -EAGAIN;
		else
			v->intval =
				1000 *
				(property == POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT ?
					 d->applied_input :
					 d->applied_charge);
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&d->io_lock);
	return ret;
}
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
	/* An inactive bind has no worker/event owner applying a zero request.
	 * Report it as unapplied; do not enable IRQs solely for an inhibit. */
	else if (!READ_ONCE(d->ready) ||
		 (!ma && !READ_ONCE(d->events_enabled) &&
		  (property == POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT ||
		   property == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT)))
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
static int d_writeable(struct power_supply *psy, enum power_supply_property p)
{
	return p == POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT ||
	       p == POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT;
}
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
#if IS_REACHABLE(CONFIG_P105_COMBINED_CURRENT_PROVIDER)
	/* Stop fence is visible before RCU drain; no callback can enqueue after
	 * cancel. Never wait RCU while holding queue/io/controller spinlocks. */
	if (d->owned_provider)
		p105_combined_provider_unregister(d->owned_provider);
#endif
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
/* Sleep permanently revokes this bind's retention assertion. Rebinding after
 * an independently reviewed fresh handoff is required; resume cannot restore
 * a prior source lease, and failed sleep stays inhibited/latched as well. */
static int d_suspend(struct device *dev)
{
	return d_stop(dev_get_drvdata(dev));
}
static int d_resume(struct device *dev)
{
	dev_warn(
		dev,
		"current controller remains stopped after sleep; no retention or source lease restored\n");
	return 0;
}
static DEFINE_SIMPLE_DEV_PM_OPS(d_pm, d_suspend, d_resume);

static void d_release(struct d1946_current *d)
{
	unsigned int i, bank;
	d_stop(d);
	if (d->psy)
		power_supply_unregister(d->psy);
	/* Parent action is exclusively owned (no IRQF_SHARED). Remove/drain it
  * before disposing nested mappings that its handler consults. */
	if (d->parent_irq >= 0) {
		free_irq(d->parent_irq, d);
		d->parent_irq = -1;
	}
	for (i = 0; i < ARRAY_SIZE(source_events); i++)
		if (d->virq[i]) {
			free_irq(d->virq[i], d);
			irq_dispose_mapping(d->virq[i]);
		}
	if (d->events_enabled) {
		mutex_lock(&d->io_lock);
		for (bank = 0; bank < D_NBANKS; bank++) {
			u8 owned = d->owned[bank];
			if (owned &&
			    d1946_update_checked(&d->io, 0x12 + bank, owned,
						 d->boot_masks[bank]))
				dev_err(d->dev,
					"owned mask restoration failed; state unknown\n");
		}
		mutex_unlock(&d->io_lock);
	}
	if (d->domain)
		irq_domain_remove(d->domain);

		/* IRQ callbacks and work are drained; no provider call can outlive close.
  * Close managed reader handles before driver-core managed-link teardown. */
#if IS_REACHABLE(CONFIG_P105_COMBINED_CURRENT_PROVIDER)
	if (d->owned_provider) {
		p105_combined_provider_close(d->owned_provider);
		d->owned_provider = NULL;
		d->provider = NULL;
	}
#endif
	if (d->source_pdev) {
		put_device(&d->source_pdev->dev);
		d->source_pdev = NULL;
	}
	if (d->battery) {
		power_supply_put(d->battery);
		d->battery = NULL;
	}
	if (d->input) {
		power_supply_put(d->input);
		d->input = NULL;
	}
}
/* All acquisitions happen before any PMU read and with work/IRQ disabled.
 * d_release() handles partial acquisition as well as a complete active bind. */
static int d_providers_acquire(struct d1946_current *d)
{
	const struct d1946_current_provider *provider =
		dev_get_platdata(d->dev);
	int ret;
	if (!owned_usb2_source) {
		if (!provider || !provider->snapshot || !provider->valid)
			return -ENODEV;
		d->provider = provider;
		d->input = power_supply_get_by_phandle(d->dev->of_node,
						       "power-supplies");
		if (IS_ERR_OR_NULL(d->input)) {
			ret = d->input ? PTR_ERR(d->input) : -EPROBE_DEFER;
			d->input = NULL;
			return ret;
		}
	}
	d->battery = power_supply_get_by_phandle(d->dev->of_node,
						 "apple,battery-supply");
	if (IS_ERR_OR_NULL(d->battery)) {
		ret = d->battery ? PTR_ERR(d->battery) : -EPROBE_DEFER;
		d->battery = NULL;
		return ret;
	}
	if (!owned_usb2_source)
		return 0;
#if IS_REACHABLE(CONFIG_P105_COMBINED_CURRENT_PROVIDER)
	{
		struct device_node *node;
		node = of_parse_phandle(d->dev->of_node,
					"apple,usb-source-controller", 0);
		if (!node)
			return -EINVAL;
		d->source_pdev = of_find_device_by_node(node);
		of_node_put(node);
		if (!d->source_pdev)
			return -EPROBE_DEFER;
		d->owned_provider = p105_combined_provider_open(
			d->dev, &d->source_pdev->dev, d->battery);
		if (IS_ERR(d->owned_provider)) {
			ret = PTR_ERR(d->owned_provider);
			d->owned_provider = NULL;
			return ret;
		}
		d->owned_source_notifier.notifier_call = d_owned_source_changed;
		ret = p105_combined_provider_register(
			d->owned_provider, &d->owned_source_notifier);
		if (ret)
			return ret;
		d->provider = p105_combined_provider_ops(d->owned_provider);
		if (!d->provider || !d->provider->snapshot ||
		    !d->provider->valid)
			return -ENODEV;
	}
	return 0;
#else
	return -EOPNOTSUPP;
#endif
}
static void d_prepare_desc(struct d1946_current *d)
{
	d->desc = (struct power_supply_desc){
		.name = dev_name(d->dev),
		.type = POWER_SUPPLY_TYPE_USB,
		.properties = d_properties,
		.num_properties = ARRAY_SIZE(d_properties),
		.get_property = d_get,
		.set_property = d_set,
		.property_is_writeable = d_writeable,
		/* No foreign supplier-work callback: power_supply_unregister only
   * drains this supply's own work, not callbacks from other suppliers.
   * All routes poll their provider and handle owned raw PMU events. */
		.external_power_changed = NULL
	};
}

static struct regmap *d_parent_map(struct device *child)
{
#if IS_ENABLED(CONFIG_APPLE_PMU_D1946_BANK0_EXPERIMENT)
	return apple_pmu_d1946_bank0_get_for_child(child);
#else
	(void)child;
	return ERR_PTR(-EOPNOTSUPP);
#endif
}
static int d_probe(struct platform_device *pdev)
{
	struct regmap *map;
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
	map = d_parent_map(&pdev->dev);
	if (IS_ERR(map))
		return PTR_ERR(map);
	d = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->dev = &pdev->dev;
	d->map = map;
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
static void d_remove(struct platform_device *pdev)
{
	d_release(platform_get_drvdata(pdev));
}
static void d_shutdown(struct platform_device *pdev)
{
	d_stop(platform_get_drvdata(pdev));
}
static const struct of_device_id d_match[] = {
	{ .compatible = "apple,d1946-current-only-experimental" },
	{}
};
MODULE_DEVICE_TABLE(of, d_match);
static struct platform_driver d_driver = {
	.probe = d_probe,
	.remove = d_remove,
	.shutdown = d_shutdown,
	.driver = { .name = "apple-d1946-current-only-experimental",
		    .of_match_table = d_match,
		    .pm = pm_sleep_ptr(&d_pm) }
};
module_platform_driver(d_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(
	"Unvalidated opt-in D1946 current-only controller with central event owner");
