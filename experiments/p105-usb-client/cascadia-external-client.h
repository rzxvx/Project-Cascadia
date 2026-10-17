/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CASCADIA_EXTERNAL_CLIENT_H
#define CASCADIA_EXTERNAL_CLIENT_H
#include <linux/types.h>

enum cascadia_external_client_state {
	CASCADIA_CLIENT_OFF,
	CASCADIA_CLIENT_ACTIVE,
	CASCADIA_CLIENT_STANDBY,
};

/*
 * Backend operations are idempotent membership updates for one owned client,
 * never blind clock/PHY reference increments or puts. The caller serializes
 * this object in its role owner's mutex; callbacks may sleep in worker context.
 * Every backend must recheck the epoch before applying its update.
 */
struct cascadia_external_client_ops {
	int (*validate_epoch)(void *context, u64 epoch);
	int (*parent_clock_mask)(void *context, u64 epoch, u32 mask);
	int (*phy_client_state)(void *context, u64 epoch, unsigned int state);
};

struct cascadia_external_client {
	const struct cascadia_external_client_ops *ops;
	void *context;
	u64 connection_epoch;
	bool initialized;
	bool permitted;
	bool touched;
	bool fault;
	bool pending;
	bool phy_known;
	bool clock_known;
	u32 parent_mask;
	u32 raw_clock_mask;
	enum cascadia_external_client_state phy_state;
	enum cascadia_external_client_state target;
};

int cascadia_external_client_init(struct cascadia_external_client *client,
	u32 raw_clock_mask);
int cascadia_external_client_prepare(struct cascadia_external_client *client,
	enum cascadia_external_client_state state, u64 epoch);
int cascadia_external_client_complete(struct cascadia_external_client *client,
	u64 epoch, int controller_error);
int cascadia_external_client_cleanup(struct cascadia_external_client *client,
	u64 epoch, bool controller_quiesced);
#endif
