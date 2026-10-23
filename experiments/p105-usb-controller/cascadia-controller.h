/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CASCADIA_CONTROLLER_H
#define CASCADIA_CONTROLLER_H
#include <linux/types.h>
struct regmap;
struct mutex;

/* Fresh zero storage only. Source-only; no probe, DT or role registration. */
struct cascadia_controller {
	struct regmap *complex;
	struct regmap *pmgr;
	struct mutex *owner_lock;
	int (*validate_epoch)(void *context, u64 epoch);
	void *context;
	bool initialized;
	bool permitted;
	bool p105_binding_verified;
	bool complete_clock_and_phy_ownership;
	bool role_lease_owned;
	u64 role_lease_epoch;
	bool gate89_owned;
	bool udc_quiesced;
	bool ehci_all_ports_quiesced;
	bool hsic_wifi_quiesced;
	bool external_power_contract_verified;
	bool touched;
	bool fault;
	int fault_error;
	unsigned int pending_operation;
	unsigned int completed_operations;
};

int cascadia_controller_init(struct cascadia_controller *c,
	struct regmap *complex, struct regmap *pmgr, struct mutex *owner_lock);
/* The complete shared owner lock must already be held by a sleepable worker. */
int cascadia_controller_mux(struct cascadia_controller *c, u64 epoch, bool host);
int cascadia_controller_ehci_reset(struct cascadia_controller *c, u64 epoch);
#endif
