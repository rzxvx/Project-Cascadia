/* Unregistered shared ALL-PMU transport, known page0 experiments only. */
#ifndef P105_BANK0_H
#define P105_BANK0_H
#include <linux/atomic.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/types.h>
struct p105_bank0_lease {
	u64 owner_generation;
	bool exclusive_all_pmu;
	bool event_owner; /* Required to consume01..09 or mutate12..1a masks. */
};
struct p105_bank0_ops {
	int (*valid)(void *ctx, const struct p105_bank0_lease *lease);
	/* Runs under io_lock: mark/quarantine or enqueue; no reentry/synchronous drains. */
	int (*hold_unknown)(void *ctx, const struct p105_bank0_lease *lease, int reason);
};
struct p105_bank0 {
	/* One context/lock for the physical PMU; no per-child bank cache. */
	struct mutex io_lock;
	atomic64_t generation;
	atomic_t page0_known;
	u64 selected_owner_generation; /* Protected by io_lock; bound to cached selection. */
	atomic_t fault;
	struct i2c_client *client;
	const struct p105_bank0_ops *ops;
	void *ctx;
	bool initialized;
	bool enabled; /* initfalse, no activation/registration code */
	bool retained_lease_valid;
	struct p105_bank0_lease retained_lease;
	int transport_error;
	int hold_error;
};
int p105_bank0_init(struct p105_bank0 *b);
void p105_bank0_invalidate(struct p105_bank0 *b);
/* Config/client/ops immutable after activation; all arguments and read buffer
 * Owner generations never repeat. Any gap in exclusive ownership invalidates
 * the cache, including a gap within the same generation. Changed owners force
 * a checked selector write before data.
 * All arguments remain live and disjoint from context/lease storage throughout this call. */
int p105_bank0_transfer(struct p105_bank0 *b, const struct p105_bank0_lease *lease,
		       bool write, unsigned int reg, void *buffer, unsigned int count);
#endif
