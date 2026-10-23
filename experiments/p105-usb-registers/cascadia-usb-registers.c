// SPDX-License-Identifier: GPL-2.0-only
/* Default-off P105 register primitives. See EVIDENCE.md for limits. */
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/regmap.h>
#include "cascadia-usb-registers.h"

#define AUDITED_CLOCK_MASK 0x3001fU
#define COMPLEX_CONTROL 0x0
#define PHY_RESET 0x08
#define PHY_TUNE1 0x40
#define PHY_TUNE2 0x44
#define PHY_HSIC_CONDITION 0x64

/* Ordered P105 usb-complex clock-gates: [88,89,90,91,87].
 * Gate87 has register index0; it is a membership-only entry, never +0x1000.
 * USB_CTL=0x64 enables only upper-mask clients10000/20000 here. Bit40000
 * and all other clock mask bits are refused rather than generalized.
 */
static const unsigned int gate_offsets[] = {
	0x1084, 0x1088, 0x1090, 0x1094, 0,
};

static int checked_epoch(struct cascadia_usb_registers *r,
	unsigned int owner, u64 epoch)
{
	int ret;

	if (!r->validate_epoch)
		return -ENODEV;
	ret = r->validate_epoch(r->context, owner, epoch);
	return ret > 0 ? -EIO : ret;
}

static int fault(struct cascadia_usb_registers *r, int ret);

static int prerequisites(struct cascadia_usb_registers *r,
	unsigned int owner, u64 epoch)
{
	int ret;

	if (!r->initialized || owner >= CASCADIA_USB_CLOCK_OWNERS)
		return -EINVAL;
	if (r->fault)
		return r->fault_error;
	if (!r->permitted || !r->complete_clock_ownership)
		return -EACCES;
	ret = checked_epoch(r, owner, epoch);
	/* Failed validation cannot forget retained physical ownership. */
	return ret ? fault(r, ret) : 0;
}

static int fault(struct cascadia_usb_registers *r, int ret)
{
	unsigned int i;

	if (ret > 0)
		ret = -EIO;
	/* A ready gate may commit membership without any local write. */
	for (i = 0; i < CASCADIA_USB_CLOCK_OWNERS; i++)
		if (r->masks[i])
			break;
	if (r->touched || i < CASCADIA_USB_CLOCK_OWNERS) {
		r->fault = true;
		r->fault_error = ret;
	}
	return ret;
}

static int write_reg(struct cascadia_usb_registers *r, struct regmap *map,
	unsigned int owner, u64 epoch, unsigned int off, u32 value)
{
	int ret = checked_epoch(r, owner, epoch);

	if (ret)
		return ret;
	/* A failed write may still have touched hardware: never guess rollback. */
	r->touched = true;
	ret = regmap_write(map, off, value);
	if (ret)
		return ret > 0 ? -EIO : ret;
	return checked_epoch(r, owner, epoch);
}

static int update_reg(struct cascadia_usb_registers *r, struct regmap *map,
	unsigned int owner, u64 epoch, unsigned int off, u32 mask, u32 value)
{
	unsigned int old;
	int ret = checked_epoch(r, owner, epoch);

	if (ret)
		return ret;
	ret = regmap_read(map, off, &old);
	if (ret)
		return ret > 0 ? -EIO : ret;
	return write_reg(r, map, owner, epoch, off, (old & ~mask) | (value & mask));
}

static int gate(struct cascadia_usb_registers *r,
	unsigned int owner, u64 epoch, unsigned int index, bool on)
{
	unsigned int off = gate_offsets[index], old, value, target = on ? 15 : 0;
	int ret, i;

	if (!off)
		return 0;
	ret = checked_epoch(r, owner, epoch);
	if (ret)
		return ret;
	ret = regmap_read(r->pmgr, off, &old);
	if (ret)
		return ret > 0 ? -EIO : ret;
	/* Audited clock_gate_switch primitive accepts the target nibble. The
	 * full PerformanceController graph is NOT implemented here. Native
	 * off-target/parent policy must be verified before releases are allowed.
	 */
	if ((old & 15) != target) {
		ret = write_reg(r, r->pmgr, owner, epoch, off,
			(old & ~0x10fU) | target);
		if (ret)
			return ret;
	}
	/* Unlike firmware's unbounded poll, error/fault is bounded and retained. */
	for (i = 0; i < 1000; i++) {
		ret = checked_epoch(r, owner, epoch);
		if (ret)
			return ret;
		ret = regmap_read(r->pmgr, off, &value);
		if (ret)
			return ret > 0 ? -EIO : ret;
		if ((value & 15) == target && ((value >> 4) & 15) == target)
			return checked_epoch(r, owner, epoch);
		udelay(1);
	}
	return -ETIMEDOUT;
}

int cascadia_usb_registers_init(struct cascadia_usb_registers *r,
	struct regmap *complex, struct regmap *pmgr, struct regmap *phy)
{
	if (!r || IS_ERR_OR_NULL(complex) || IS_ERR_OR_NULL(pmgr) ||
	    IS_ERR_OR_NULL(phy))
		return -EINVAL;
	if (r->initialized)
		return -EALREADY;
	/* Fresh zero storage, preserving caller metadata rather than memset(). */
	mutex_init(&r->lock);
	r->complex = complex;
	r->pmgr = pmgr;
	r->phy = phy;
	r->initialized = true;
	return 0;
}

int cascadia_usb_clock_update(struct cascadia_usb_registers *r,
	unsigned int owner, u64 epoch, u32 mask)
{
	u32 old = 0, next = 0, bit, upper, control_bit;
	unsigned int i;
	int ret;

	if (!r || !r->initialized || owner >= CASCADIA_USB_CLOCK_OWNERS ||
	    mask & ~AUDITED_CLOCK_MASK)
		return -EINVAL;
	/* Original subclass forces10000 for mask60070 before membership. */
	if (mask & 0x60070U)
		mask |= 0x10000U;
	mutex_lock(&r->lock);
	ret = prerequisites(r, owner, epoch);
	if (ret)
		goto out;
	for (i = 0; i < CASCADIA_USB_CLOCK_OWNERS; i++) {
		old |= r->masks[i];
		next |= i == owner ? mask : r->masks[i];
	}
	if (!r->gate_release_verified && (old & ~next)) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	r->pending_owner = owner;
	r->pending_mask = mask;
	/* Firmware opens USB_CTL clocks before low gate transitions. */
	for (i = 0; i < 2; i++) {
		upper = 0x10000U << i;
		control_bit = 0x40U >> i;
		if (!(old & upper) && (next & upper)) {
			ret = update_reg(r, r->complex, owner, epoch,
				COMPLEX_CONTROL, control_bit, 0);
			if (ret)
				goto failed;
		}
	}
	for (i = 0; i < 5; i++) {
		bit = 1U << i;
		if ((old ^ next) & bit) {
			ret = gate(r, owner, epoch, i, next & bit);
			if (ret)
				goto failed;
		}
	}
	/* Firmware closes USB_CTL clocks only after low gate transitions. */
	for (i = 0; i < 2; i++) {
		upper = 0x10000U << i;
		control_bit = 0x40U >> i;
		if ((old & upper) && !(next & upper)) {
			ret = update_reg(r, r->complex, owner, epoch,
				COMPLEX_CONTROL, control_bit, control_bit);
			if (ret)
				goto failed;
		}
	}
	ret = checked_epoch(r, owner, epoch);
	if (ret)
		goto failed;
	r->masks[owner] = mask;
	goto out;
failed:
	ret = fault(r, ret);
out:
	mutex_unlock(&r->lock);
	return ret;
}

int cascadia_usb_host_tune(struct cascadia_usb_registers *r,
	unsigned int owner, u64 epoch, bool host, unsigned int leased_power_level,
	bool hsic_enabled, bool all_controllers_quiesced, bool real_power_lease)
{
	int ret;

	if (!r || !r->initialized || owner >= CASCADIA_USB_CLOCK_OWNERS)
		return -EINVAL;
	mutex_lock(&r->lock);
	ret = prerequisites(r, owner, epoch);
	if (ret)
		goto out;
	if (!all_controllers_quiesced || !real_power_lease ||
	    leased_power_level < 2 || leased_power_level > 4) {
		ret = -EOPNOTSUPP;
		goto out;
	}
	ret = write_reg(r, r->phy, owner, epoch, PHY_TUNE1, host ? 0x54f : 0x549);
	if (ret)
		goto failed;
	ret = write_reg(r, r->phy, owner, epoch, PHY_TUNE2, host ? 0x6df3 : 0x2ff3);
	if (ret)
		goto failed;
	ret = update_reg(r, r->phy, owner, epoch, PHY_RESET, 1, 1);
	if (ret)
		goto failed;
	udelay(20);
	ret = update_reg(r, r->phy, owner, epoch, PHY_RESET, 1, 0);
	if (ret)
		goto failed;
	udelay(1000);
	/* hsic_enabled is real owner state, not guessed from a generic PHY ID. */
	if (!hsic_enabled) {
		ret = update_reg(r, r->phy, owner, epoch, PHY_HSIC_CONDITION,
			2, host ? 2 : 0);
		if (ret)
			goto failed;
	}
	ret = checked_epoch(r, owner, epoch);
	if (ret)
		goto failed;
	goto out;
failed:
	ret = fault(r, ret);
out:
	mutex_unlock(&r->lock);
	return ret;
}
