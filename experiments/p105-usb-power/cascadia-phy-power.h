/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CASCADIA_PHY_POWER_H
#define CASCADIA_PHY_POWER_H
#include <linux/types.h>
struct regmap;
struct clk;
struct mutex;

/* Caller serializes ALL clock, PHY, controller and connector users through
 * shared_lock, and holds a verified transition lease. No registration exists.
 */
struct cascadia_phy_power {
	struct regmap *phy;
	struct regmap *complex;
	struct clk *usbphyclk;
	struct mutex *shared_lock;
	int (*validate_epoch)(void *context, u64 epoch);
	int (*clock_membership)(void *context, u64 epoch, u32 mask);
	void *context;
	unsigned long adopted_clock_hz;
	unsigned int level;
	unsigned int pending_level;
	bool initialized;
	bool permitted;
	bool p105_binding_verified;
	bool power_gates_absent_verified;
	bool complete_owned_membership;
	bool all_owners_quiesced;
	bool transition_lease_verified;
	bool state_adopted;
	bool touched;
	bool fault;
	int fault_error;
};
int cascadia_phy_power_init(struct cascadia_phy_power *p, struct regmap *phy,
	struct regmap *complex, struct clk *usbphyclk, struct mutex *shared_lock);
int cascadia_phy_power_transition(struct cascadia_phy_power *p,
	u64 epoch, unsigned int target);
#endif
