/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CASCADIA_NATIVE_CLOCK_H
#define CASCADIA_NATIVE_CLOCK_H
#include <linux/types.h>
struct regmap;
struct mutex;
#define CASCADIA_NATIVE_NODES 8
/* Own RAM, not the Apple object layout. Counts are native <56 references. */
struct cascadia_native_state {
	u32 refs[6]; /* IDs 2,3,4,5,40,42 */
	u32 owners[2]; /* IDs87,89: first/last aggregate of owned clients */
	u8 foreign_roots; /* protected pre-existing87/89 leaf active bits */
};
struct cascadia_native_adoption {
	bool p105_source_binding_verified;
	bool dynamic_provider_association_verified;
	bool all_global_owners_adopted;
	bool inherited_hardware_verified;
	bool all_shared_clients_quiesced;
	bool gate_power_policy_owned;
	u64 epoch;
	u64 zero_off_gates; /* Only gate indices32,34,38 are supported. */
	struct cascadia_native_state baseline;
};
struct cascadia_native_clock {
	struct regmap *pmgr;
	struct mutex *owner_lock;
	int (*validate_epoch)(void *context, u64 epoch);
	void *context;
	bool initialized;
	bool permitted;
	bool adopted;
	bool touched;
	bool fault;
	int fault_error;
	unsigned int poll_limit;
	unsigned int pending_root;
	bool pending_enable;
	bool pending_valid;
	unsigned int completed_operations;
	struct cascadia_native_adoption certificate;
	struct cascadia_native_state state;
	struct cascadia_native_state pending;
};
/* Fresh zero storage only; reinitialization is refused. No registration/DT. */
int cascadia_native_clock_init(struct cascadia_native_clock *c,
	struct regmap *pmgr, struct mutex *owner_lock);
/* Complete global owner mutex must already be held in a sleepable worker. */
int cascadia_native_clock_adopt(struct cascadia_native_clock *c,
	const struct cascadia_native_adoption *adoption);
int cascadia_native_clock_request(struct cascadia_native_clock *c, u64 epoch,
	unsigned int root, unsigned int owner, bool enable);
#endif
