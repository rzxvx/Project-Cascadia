/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CASCADIA_RESET_CLIENT_H
#define CASCADIA_RESET_CLIENT_H
#include <linux/types.h>
struct regmap;
struct mutex;
#define CASCADIA_RESET_OWNERS 16
struct cascadia_reset_client {
	struct regmap *phy;
	struct mutex *shared_lock;
	int (*validate_epoch)(void *context, u64 epoch);
	/* Real identity membership for a dedicated temporary power-level4
	 * owner. Must propagate partial/error state, never a raw PHY get/put.
	 */
	int (*temporary_power_mask)(void *context, u64 epoch, u32 mask);
	void *context;
	u8 memberships[CASCADIA_RESET_OWNERS];
	u64 owner_epochs[CASCADIA_RESET_OWNERS];
	u8 pending_mask;
	unsigned int pending_owner;
	bool initialized;
	bool permitted;
	bool p105_binding_verified;
	bool complete_standard_group_owned;
	bool all_owners_quiesced;
	bool state_adopted;
	bool touched;
	bool fault;
	bool lease_maybe_owned;
	bool lease_known;
	int fault_error;
};
int cascadia_reset_client_init(struct cascadia_reset_client *c,
	struct regmap *phy, struct mutex *shared_lock);
int cascadia_reset_client_update(struct cascadia_reset_client *c,
	u64 epoch, unsigned int owner, unsigned int kind, bool add);
#endif
