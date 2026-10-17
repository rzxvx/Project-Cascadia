// SPDX-License-Identifier: GPL-2.0-only
/* Source-only P105 primitives; this file registers no controller or role. */
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/lockdep.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include "cascadia-controller.h"

#define EHCI_GATE 0x1088
#define RESET_BIT 0x80000000U

int cascadia_controller_init(struct cascadia_controller *c,
	struct regmap *complex, struct regmap *pmgr, struct mutex *owner_lock)
{
	if (!c || IS_ERR_OR_NULL(complex) || IS_ERR_OR_NULL(pmgr) || IS_ERR_OR_NULL(owner_lock))
		return -EINVAL;
	if (c->initialized)
		return -EALREADY;
	memset(c, 0, sizeof(*c));
	c->complex = complex;
	c->pmgr = pmgr;
	c->owner_lock = owner_lock;
	c->initialized = true;
	return 0;
}

static int epoch_check(struct cascadia_controller *c, u64 epoch)
{
	int ret = c->validate_epoch(c->context, epoch);

	return ret > 0 ? -EIO : ret;
}

static int fault(struct cascadia_controller *c, int ret)
{
	c->fault = true;
	c->fault_error = ret < 0 ? ret : -EIO;
	return c->fault_error;
}

static int prepare(struct cascadia_controller *c, u64 epoch)
{
	int ret;

	if (!c || !c->initialized)
		return -EINVAL;
	lockdep_assert_held(c->owner_lock);
	if (c->fault)
		return c->fault_error;
	if (!c->permitted)
		return -EACCES;
	if (!c->p105_binding_verified || !c->complete_clock_and_phy_ownership ||
	    !c->role_lease_owned || !c->gate89_owned || !c->udc_quiesced ||
	    !c->ehci_all_ports_quiesced || !c->hsic_wifi_quiesced ||
	    !c->external_power_contract_verified)
		return -EOPNOTSUPP;
	if (!c->validate_epoch)
		return (c->touched || c->completed_operations) ?
			fault(c, -ENODEV) : -ENODEV;
	if (epoch != c->role_lease_epoch)
		return -ESTALE;
	ret = epoch_check(c, epoch);
	if (ret && (c->touched || c->completed_operations))
		return fault(c, ret);
	return ret;
}

static int read_checked(struct cascadia_controller *c, struct regmap *map,
	u64 epoch, unsigned int off, unsigned int *value)
{
	int ret = epoch_check(c, epoch);

	if (ret)
		return ret;
	ret = regmap_read(map, off, value);
	if (ret)
		return ret;
	return epoch_check(c, epoch);
}

static int write_checked(struct cascadia_controller *c, struct regmap *map,
	u64 epoch, unsigned int off, unsigned int value)
{
	int ret = epoch_check(c, epoch);

	if (ret)
		return ret;
	/* A provider error can follow a physical write: never assume rollback. */
	c->touched = true;
	ret = regmap_write(map, off, value);
	if (ret)
		return ret;
	return epoch_check(c, epoch);
}

static int gate_ready(struct cascadia_controller *c, u64 epoch,
	unsigned int *value)
{
	int ret = read_checked(c, c->pmgr, epoch, EHCI_GATE, value);

	if (ret)
		return fault(c, ret);
	if (*value & RESET_BIT)
		ret = -EBUSY;
	/* Target and acknowledgement nibbles must both be on (0xf). */
	else if ((*value & 0xff) != 0xff)
		ret = -EHOSTDOWN;
	else
		return 0;
	/* A completed/touched owner cannot adopt an intervening reset/gate gap
	 * by retrying the same lease when the register later looks ready. */
	return (c->touched || c->completed_operations) ? fault(c, ret) : ret;
}

static int complete(struct cascadia_controller *c, u64 epoch)
{
	int ret = epoch_check(c, epoch);

	if (ret)
		return fault(c, ret);
	c->pending_operation = 0;
	c->completed_operations++;
	return 0;
}

int cascadia_controller_mux(struct cascadia_controller *c, u64 epoch, bool host)
{
	unsigned int gate, value;
	int ret = prepare(c, epoch);

	if (ret)
		return ret;
	c->pending_operation = host ? 1 : 2;
	ret = gate_ready(c, epoch, &gate);
	if (ret)
		return ret;
	/* Original selector block clears bit0, reads again, then ORs host0/1. */
	ret = read_checked(c, c->complex, epoch, 0, &value);
	if (!ret)
		ret = write_checked(c, c->complex, epoch, 0, value & ~1U);
	if (!ret)
		ret = read_checked(c, c->complex, epoch, 0, &value);
	if (!ret)
		ret = write_checked(c, c->complex, epoch, 0, value | (host ? 1U : 0U));
	if (ret)
		return fault(c, ret);
	return complete(c, epoch);
}

int cascadia_controller_ehci_reset(struct cascadia_controller *c, u64 epoch)
{
	unsigned int saved;
	int ret = prepare(c, epoch);

	if (ret)
		return ret;
	c->pending_operation = 3;
	ret = gate_ready(c, epoch, &saved);
	if (ret)
		return ret;
	/* Actual ARST index0x22: PMGR0x1000+4*index, saved-bit pulse1us. */
	ret = write_checked(c, c->pmgr, epoch, EHCI_GATE, saved | RESET_BIT);
	if (ret)
		return fault(c, ret);
	udelay(1);
	ret = epoch_check(c, epoch);
	if (!ret)
		ret = write_checked(c, c->pmgr, epoch, EHCI_GATE, saved);
	if (ret)
		return fault(c, ret);
	return complete(c, epoch);
}
