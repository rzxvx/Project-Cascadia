/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CASCADIA_ACCESSORY_ROLE_H
#define CASCADIA_ACCESSORY_ROLE_H
#include <linux/types.h>

/* The masks are policy inputs, not adapter identities or GPIO values. */
struct cascadia_accessory_role_policy {
	u16 value_mask;
	u16 index_mask;
	bool allow_switch;
	u64 connection_epoch;
	void *context;
	int (*authorize)(void *context, u64 epoch, u16 value, u16 index);
	int (*request_switch)(void *context, u64 epoch, const u8 setup[8]);
};

/*
 * Transport-only: 4 for an IN response, 0 for accepted OUT, negative errno for
 * stall/reject. -EOPNOTSUPP delegates to the enclosing gadget setup handler.
 * No hardware backend is supplied. Default-zero policy refuses all switches.
 * Callbacks must not sleep or retain setup, and must validate the attach epoch.
 */
int cascadia_accessory_role_setup(const u8 *setup, size_t setup_length,
	const struct cascadia_accessory_role_policy *policy, u8 response[4]);
#endif
