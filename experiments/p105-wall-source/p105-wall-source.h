/* Source-only experiment: no registration, no DT, no current authorization. */
#ifndef P105_WALL_SOURCE_H
#define P105_WALL_SOURCE_H
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/types.h>

struct p105_wall_lease {
	u64 generation;
	/* Provider proves exclusive Tristar routes + PMU ADC6 ownership and restore. */
	bool checked_restore_available;
	bool orientation_seen;
	u8 orientation;
};

/* All int callbacks return0 or a negative errno. acquire negative errors
 * retain no lease; partial acquisition must be rolled back by the provider.
 * Positive acquisition is a protocol fault with unknown retained ownership. */
struct p105_wall_ops {
	int (*acquire)(void *ctx, struct p105_wall_lease *lease);
	int (*validate)(void *ctx, const struct p105_wall_lease *lease);
	int (*adc6_raw)(void *ctx, const struct p105_wall_lease *lease, u16 *raw);
	int (*settle)(void *ctx, const struct p105_wall_lease *lease, u32 original_interval);
	/* Must drain ADC, restore previous owner/routing, report errors; never route0-only. */
	int (*restore_release)(void *ctx, const struct p105_wall_lease *lease);
};

struct p105_wall_source {
	struct mutex io_lock;
	atomic64_t generation;
	atomic_t fault;
	struct regmap *pmu;
	struct regmap *tristar;
	const struct p105_wall_ops *ops;
	void *ctx;
	bool initialized;
	bool lease_uncertain;
	struct p105_wall_lease held_lease;
	int held_error;
	bool enabled; /* init always false; no public activation/registration code. */
};

struct p105_wall_observation {
	u8 source_type; /* Original ordinals, not a physical permission/cable guarantee. */
	u8 divider_id;
	u32 first_scalar;
	u32 second_scalar;
	u32 third_scalar;
	bool third_sample;
	u64 generation;
	/* Deliberately no usable_current/current_permission field. */
};

/* Fresh all-zero storage exactly once; no reset/reinit to forget ownership. */
int p105_wall_init(struct p105_wall_source *s);
void p105_wall_invalidate(struct p105_wall_source *s, bool fault);
int p105_wall_classify(u32 first, u32 second, bool third_valid, u32 third,
		       struct p105_wall_observation *out);
/* Resources/config stay live and immutable; output must be disjoint from
 * context storage. Callbacks must not reenter the held io_lock. */
int p105_wall_capture(struct p105_wall_source *s, struct p105_wall_observation *out);
#endif
