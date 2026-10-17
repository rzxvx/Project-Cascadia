/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CASCADIA_THS_IDBUS_H
#define CASCADIA_THS_IDBUS_H
#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#define CASCADIA_THS_WINDOW 32
/* Must prove exclusive physical IDbus ownership for this owner/epoch. */
struct cascadia_ths_lease {
	int (*validate)(void *cookie, u64 owner, u64 epoch);
	void *cookie;
};
enum cascadia_ths_state { CASCADIA_THS_IDLE, CASCADIA_THS_PENDING,
	CASCADIA_THS_UNKNOWN };
struct cascadia_ths_idbus {
	struct regmap *map;
	struct mutex *owner_lock;
	struct cascadia_ths_lease lease;
	u64 owner;
	u64 epoch;
	u64 request;
	u8 pending_rx;
	enum cascadia_ths_state state;
	bool initialized;
	bool touched;
	int error;
	atomic_t revoked;
};
/* Fresh zeroed storage only. All resources/lease must outlive the context. */
int cascadia_ths_init(struct cascadia_ths_idbus *ctx, struct regmap *map,
		      struct mutex *owner_lock, const struct cascadia_ths_lease *lease,
		      u64 epoch);
int cascadia_ths_submit(struct cascadia_ths_idbus *ctx, u64 owner, u64 epoch,
			const u8 *tx, size_t tx_len, size_t rx_len, u64 *request);
/* Irreversible software revocation only; never resets/releases hardware. */
int cascadia_ths_invalidate(struct cascadia_ths_idbus *ctx);
/* One status read per call; no IRQ registration, automatic polling or resend. */
int cascadia_ths_receive(struct cascadia_ths_idbus *ctx, u64 owner, u64 epoch,
			 u64 request, u8 *reply, size_t capacity, size_t *length);
#endif
