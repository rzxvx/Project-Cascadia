/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CASCADIA_USB_REGISTERS_H
#define CASCADIA_USB_REGISTERS_H
#include <linux/types.h>
#include <linux/mutex.h>
struct regmap;

#define CASCADIA_USB_CLOCK_OWNERS 16

/*
 * Source-only register primitives, not a connector/role driver. Fresh zero
 * storage only. No probe, mapping, DT, gadget, HCD or VBUS provider is registered.
 * All existing clients must be migrated into this shared owner before use.
 */
struct cascadia_usb_registers {
	struct mutex lock;
	struct regmap *complex;
	struct regmap *pmgr;
	struct regmap *phy;
	int (*validate_epoch)(void *context, unsigned int owner, u64 epoch);
	void *context;
	u32 masks[CASCADIA_USB_CLOCK_OWNERS];
	u32 pending_mask;
	unsigned int pending_owner;
	bool initialized;
	bool permitted;
	bool complete_clock_ownership;
	bool gate_release_verified;
	bool touched;
	bool fault;
	int fault_error;
};

int cascadia_usb_registers_init(struct cascadia_usb_registers *r,
	struct regmap *complex, struct regmap *pmgr, struct regmap *phy);
int cascadia_usb_clock_update(struct cascadia_usb_registers *r,
	unsigned int owner, u64 epoch, u32 mask);

/*
 * Caller must hold a real shared power-level lease AND have quiesced every
 * affected controller. These inputs attest an external owner contract; this
 * primitive does not discover, acquire or verify that hardware lifecycle.
 * It refuses level<2 (firmware would leave reset asserted at level1).
 */
int cascadia_usb_host_tune(struct cascadia_usb_registers *r,
	unsigned int owner, u64 epoch, bool host, unsigned int leased_power_level,
	bool hsic_enabled, bool all_controllers_quiesced, bool real_power_lease);
#endif
