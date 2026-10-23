/* SPDX-License-Identifier: GPL-2.0 */
#ifndef D1946_CURRENT_PROVIDER_H
#define D1946_CURRENT_PROVIDER_H
#include <linux/types.h>
struct power_supply;
/* The actual owned USB2/gauge adapter is available separately. It must publish
 * a coherent generation covering source permission AND battery telemetry.
 * valid() is nonblocking, cannot call back into this driver, and must reject
 * detach/suspend/role change/measurement invalidation immediately. */
struct d1946_snapshot {
	u64 generation, sampled_ns, valid_until_ns;
	int online, budget_ua, present, voltage_uv, temp_decic;
};
struct d1946_current_provider {
	void *context;
	int (*snapshot)(void *, struct power_supply *, struct power_supply *,
			struct d1946_snapshot *);
	bool (*valid)(void *, const struct d1946_snapshot *);
};
#endif
