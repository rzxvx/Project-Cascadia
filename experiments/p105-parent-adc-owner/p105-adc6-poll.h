// SPDX-License-Identifier: GPL-2.0-only
/* Unregistered ADC6 research candidate. No device, IRQ or current authorization. */
#ifndef P105_ADC6_POLL_H
#define P105_ADC6_POLL_H
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/types.h>
struct p105_adc6_lease {
	u64 owner_generation;
	/* Real owner must prove ALL-ADC exclusion, no outstanding/late conversion. */
	bool exclusive_all_adc;
	bool quiescence_proven;
};
/* Callback results use0/negative errno; positive values are protocol errors. */
struct p105_adc6_ops {
	int (*valid)(void *ctx, const struct p105_adc6_lease *lease);
	/* Quarantine all ADC users on uncertainty; lifetime held through this call. */
	int (*hold_unknown)(void *ctx, const struct p105_adc6_lease *lease, int reason);
};
struct p105_adc6 {
	struct mutex io_lock;
	atomic64_t generation;
	atomic_t unknown;
	struct regmap *pmu;
	const struct p105_adc6_ops *ops;
	void *ctx;
	bool initialized;
	bool enabled; /* false at init, no activation/registration implementation */
	bool retained_lease_valid;
	struct p105_adc6_lease retained_lease;
	int first_error;
	int cleanup_error;
	int hold_error;
};
struct p105_adc6_sample {
	u16 raw;
	u64 generation;
	u64 started_ns;
	u64 completed_ns;
};
int p105_adc6_init(struct p105_adc6 *a);
void p105_adc6_invalidate(struct p105_adc6 *a);
/* Initialized resources/config stay live and immutable; output must be
 * disjoint from context/lease storage. Callbacks must not reenter io_lock. */
int p105_adc6_read(struct p105_adc6 *a, const struct p105_adc6_lease *lease,
		   struct p105_adc6_sample *out);
#endif
