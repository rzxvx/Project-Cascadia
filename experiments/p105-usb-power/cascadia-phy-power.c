// SPDX-License-Identifier: GPL-2.0-only
/* Private P105 staged PHY power primitive. No DT, driver or role registration. */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/lockdep.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include "cascadia-phy-power.h"

static int epoch(struct cascadia_phy_power *p, u64 value)
{
	int ret = p->validate_epoch(p->context, value);

	return ret > 0 ? -EIO : ret;
}

static int write_reg(struct cascadia_phy_power *p, struct regmap *map,
	u64 value, unsigned int off, u32 data)
{
	int ret = epoch(p, value);

	if (ret)
		return ret;
	/* A failed I/O can be partially applied. Do not invent rollback. */
	p->touched = true;
	ret = regmap_write(map, off, data);
	if (ret)
		return ret > 0 ? -EIO : ret;
	return epoch(p, value);
}

static int update_reg(struct cascadia_phy_power *p, struct regmap *map,
	u64 value, unsigned int off, u32 mask, u32 data)
{
	unsigned int old;
	int ret = epoch(p, value);

	if (ret)
		return ret;
	ret = regmap_read(map, off, &old);
	if (ret)
		return ret > 0 ? -EIO : ret;
	return write_reg(p, map, value, off, (old & ~mask) | (data & mask));
}

static int clocks(struct cascadia_phy_power *p, u64 value, u32 mask)
{
	int ret = epoch(p, value);

	if (ret)
		return ret;
	/* This callback must be a real idempotent membership backend. Missing
	 * native parent/gate-release support must remain an error, not success.
	 */
	p->touched = true;
	ret = p->clock_membership(p->context, value, mask);
	if (ret)
		return ret > 0 ? -EIO : ret;
	return epoch(p, value);
}

static int wait_us(struct cascadia_phy_power *p, u64 value, unsigned int usecs)
{
	int ret = epoch(p, value);

	if (ret)
		return ret;
	udelay(usecs);
	return epoch(p, value);
}

static int rate_code(struct cascadia_phy_power *p)
{
	/* Actual named usbphyclk input, not a guessed fixed DT/UART rate. */
	switch (clk_get_rate(p->usbphyclk)) {
	case 12000000:
		return 0;
	case 24000000:
		return 1;
	case 48000000:
		return 2;
	default:
		return -EINVAL;
	}
}

static int up(struct cascadia_phy_power *p, u64 value, unsigned int code)
{
	int ret;

	switch (p->level) {
	case 0:
		ret = clocks(p, value, 0x20010);
		if (ret)
			return ret;
		/* P105 power-gates property is verified absent. No required
		 * provider operation is mocked as success here.
		 */
		ret = update_reg(p, p->phy, value, 8, 0x3f, 0x3f);
		if (ret)
			return ret;
		ret = write_reg(p, p->phy, value, 0x60, 0x2e0);
		if (ret)
			return ret;
		ret = write_reg(p, p->phy, value, 0, 9 | (3 << 1));
		if (ret)
			return ret;
		ret = write_reg(p, p->phy, value, 4, 0x10 | code);
		if (ret)
			return ret;
		ret = write_reg(p, p->phy, value, 0x40, 0x549);
		if (ret)
			return ret;
		ret = write_reg(p, p->phy, value, 0x44, 0x2ff3);
		if (ret)
			return ret;
		/* Actual P105 phy-tuning-val is zero: skip +0x5c write. */
		ret = wait_us(p, value, 20);
		if (ret)
			return ret;
		ret = update_reg(p, p->phy, value, 8, 2, 0);
		if (ret)
			return ret;
		ret = wait_us(p, value, 1000);
		if (ret)
			return ret;
		return clocks(p, value, 0);
	case 1:
		ret = clocks(p, value, 0x20010);
		if (ret)
			return ret;
		/* Exact actual Arbitrator+34c(argument0): set complex bit80. */
		ret = update_reg(p, p->complex, value, 0, 0x80, 0x80);
		if (ret)
			return ret;
		ret = update_reg(p, p->phy, value, 0, 1, 0);
		if (ret)
			return ret;
		ret = wait_us(p, value, 20);
		if (ret)
			return ret;
		ret = update_reg(p, p->phy, value, 8, 1, 0);
		if (ret)
			return ret;
		return wait_us(p, value, 1000);
	case 2:
		return update_reg(p, p->phy, value, 0, 8, 0);
	case 3:
		return epoch(p, value);
	default:
		return -EINVAL;
	}
}

static int down(struct cascadia_phy_power *p, u64 value)
{
	int ret;

	switch (p->level) {
	case 4:
		return epoch(p, value);
	case 3:
		return update_reg(p, p->phy, value, 0, 8, 8);
	case 2:
		ret = update_reg(p, p->phy, value, 0, 1, 1);
		if (ret)
			return ret;
		ret = update_reg(p, p->phy, value, 8, 1, 1);
		if (ret)
			return ret;
		/* Exact actual Arbitrator+34c(argument1): clear complex bit80. */
		ret = update_reg(p, p->complex, value, 0, 0x80, 0);
		if (ret)
			return ret;
		return clocks(p, value, 0);
	case 1:
		ret = clocks(p, value, 0x20010);
		if (ret)
			return ret;
		ret = write_reg(p, p->phy, value, 0, 0x19);
		if (ret)
			return ret;
		ret = update_reg(p, p->phy, value, 8, 0x3f, 0x3f);
		if (ret)
			return ret;
		/* No power-gates property on verified P105: no operation. */
		return clocks(p, value, 0);
	default:
		return -EINVAL;
	}
}

int cascadia_phy_power_init(struct cascadia_phy_power *p, struct regmap *phy,
	struct regmap *complex, struct clk *usbphyclk, struct mutex *shared_lock)
{
	if (!p || IS_ERR_OR_NULL(phy) || IS_ERR_OR_NULL(complex) ||
	    IS_ERR_OR_NULL(usbphyclk) || IS_ERR_OR_NULL(shared_lock))
		return -EINVAL;
	if (p->initialized)
		return -EALREADY;
	/* Caller provides fresh zeroed storage; no state is erased on reinit. */
	p->phy = phy;
	p->complex = complex;
	p->usbphyclk = usbphyclk;
	p->shared_lock = shared_lock;
	p->initialized = true;
	return 0;
}

int cascadia_phy_power_transition(struct cascadia_phy_power *p,
	u64 value, unsigned int target)
{
	unsigned int next;
	int ret, code;
	unsigned long hz;

	if (!p || !p->initialized || target > 4 || p->level > 4)
		return -EINVAL;
	/* The role-owner worker holds this across every shared backend call.
	 * Never run from IRQ/spinlock context; Regmap/providers may sleep.
	 */
	lockdep_assert_held(p->shared_lock);
	if (p->fault)
		return p->fault_error;
	if (!p->permitted)
		return -EACCES;
	if (!p->p105_binding_verified || !p->power_gates_absent_verified ||
	    !p->complete_owned_membership || !p->all_owners_quiesced ||
	    !p->transition_lease_verified || !p->state_adopted)
		return -EOPNOTSUPP;
	if (!p->validate_epoch || !p->clock_membership) {
		ret = -ENODEV;
		goto failed;
	}
	ret = epoch(p, value);
	if (ret)
		goto failed;
	code = rate_code(p);
	if (code < 0) {
		ret = code;
		goto failed;
	}
	hz = 12000000UL << code;
	/* An already active PHY needs a verified rate in its adoption record. */
	if (p->level && !p->adopted_clock_hz)
		return -EOPNOTSUPP;
	if (p->level && p->adopted_clock_hz != hz) {
		p->fault = true;
		p->fault_error = -ESTALE;
		return -ESTALE;
	}
	ret = epoch(p, value);
	if (ret)
		goto failed;
	p->pending_level = target;
	while (p->level != target) {
		next = p->level < target ? p->level + 1 : p->level - 1;
		ret = p->level < target ? up(p, value, code) : down(p, value);
		if (ret)
			goto failed;
		/* A changed clock invalidates even a completed register stage. */
		if (rate_code(p) != code) {
			ret = -ESTALE;
			goto failed;
		}
		ret = epoch(p, value);
		if (ret)
			goto failed;
		p->adopted_clock_hz = hz;
		p->level = next;
	}
	ret = epoch(p, value);
	if (!ret)
		return 0;
failed:
	if (p->touched || p->level) {
		p->fault = true;
		p->fault_error = ret;
	}
	return ret;
}
