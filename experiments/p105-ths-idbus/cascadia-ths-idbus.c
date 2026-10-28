// SPDX-License-Identifier: GPL-2.0
/* Unregistered THS7383 primitive. External lease/IRQ owner is not provided. */
#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/string.h>
#include "cascadia-ths-idbus.h"
/* Unique positive tokens for this allocator/code lifetime, never wrapped. */
static atomic64_t ths_tokens = ATOMIC64_INIT(0);
static int ths_token(u64 *token)
{
	s64 old;

	for (;;) {
		old = atomic64_read(&ths_tokens);
		if (old < 0 || old == 0x7fffffffffffffffLL)
			return -EOVERFLOW;
		if (atomic64_cmpxchg(&ths_tokens, old, old + 1) == old) {
			*token = old + 1;
			return 0;
		}
	}
}
static bool ths_overlap(const void *a, size_t na, const void *b, size_t nb)
{
	unsigned long x = (unsigned long)a, y = (unsigned long)b;

	return x <= y ? y - x < na : x - y < nb;
}
static int ths_unknown(struct cascadia_ths_idbus *ctx, int error)
{
	ctx->state = CASCADIA_THS_UNKNOWN;
	ctx->error = error < 0 ? error : -EPROTO;
	return ctx->error;
}
int cascadia_ths_invalidate(struct cascadia_ths_idbus *ctx)
{
	if (IS_ERR_OR_NULL(ctx) || !ctx->initialized)
		return -EINVAL;
	atomic_set(&ctx->revoked, 1);
	return 0;
}
static int ths_lease(struct cascadia_ths_idbus *ctx)
{
	u64 owner = ctx->owner, epoch = ctx->epoch, request = ctx->request;
	u8 pending_rx = ctx->pending_rx;
	enum cascadia_ths_state state = ctx->state;
	int error;

	if (ctx->state == CASCADIA_THS_UNKNOWN)
		return ctx->error;
	if (atomic_read(&ctx->revoked))
		return ths_unknown(ctx, -ESTALE);
	error = ctx->lease.validate ?
		ctx->lease.validate(ctx->lease.cookie, owner, epoch) : -EACCES;
	/* A validator can itself deliver a connection-invalidated event. */
	if (atomic_read(&ctx->revoked) || owner != ctx->owner ||
	    epoch != ctx->epoch || request != ctx->request ||
	    pending_rx != ctx->pending_rx || state != ctx->state)
		return ths_unknown(ctx, -ESTALE);
	if (error)
		return ctx->touched ? ths_unknown(ctx, error) :
		       (error < 0 ? error : -EPROTO);
	return 0;
}
static int ths_identity(struct cascadia_ths_idbus *ctx, u64 owner, u64 epoch)
{
	if (IS_ERR_OR_NULL(ctx) || !ctx->initialized)
		return -EINVAL;
	if (owner != ctx->owner || epoch != ctx->epoch)
		return -ESTALE;
	return 0;
}
int cascadia_ths_init(struct cascadia_ths_idbus *ctx, struct regmap *map,
		      struct mutex *owner_lock, const struct cascadia_ths_lease *lease,
		      u64 epoch)
{
	struct cascadia_ths_lease saved = { };
	u64 owner;
	int error;

	if (IS_ERR_OR_NULL(ctx) || IS_ERR_OR_NULL(map) || IS_ERR_OR_NULL(owner_lock) ||
	    IS_ERR(lease))
		return -EINVAL;
	if (memchr_inv(ctx, 0, sizeof(*ctx)))
		return -EALREADY;
	if (regmap_get_val_bytes(map) != 1)
		return -EINVAL;
	if (lease)
		saved = *lease;
	error = ths_token(&owner);
	if (error)
		return error;
	memset(ctx, 0, sizeof(*ctx));
	ctx->map = map;
	ctx->owner_lock = owner_lock;
	ctx->lease = saved;
	ctx->owner = owner;
	ctx->epoch = epoch;
	atomic_set(&ctx->revoked, 0);
	ctx->initialized = true;
	return 0;
}
int cascadia_ths_submit(struct cascadia_ths_idbus *ctx, u64 owner, u64 epoch,
			const u8 *tx, size_t tx_len, size_t rx_len, u64 *request)
{
	u8 bytes[CASCADIA_THS_WINDOW];
	u64 token;
	int error;

	if (!request || (ctx && ths_overlap(request, sizeof(*request),
					     ctx, sizeof(*ctx))) ||
	    (tx && ths_overlap(request, sizeof(*request), tx, tx_len)))
		return -EINVAL;
	*request = 0;
	if (!tx || !tx_len || tx_len > CASCADIA_THS_WINDOW ||
	    !rx_len || rx_len > CASCADIA_THS_WINDOW)
		return -EINVAL;
	error = ths_identity(ctx, owner, epoch);
	if (error)
		return error;
	memcpy(bytes, tx, tx_len);
	mutex_lock(ctx->owner_lock);
	error = ths_lease(ctx);
	if (error)
		goto out;
	if (ctx->state != CASCADIA_THS_IDLE) {
		error = -EBUSY;
		goto out;
	}
	error = ths_token(&token);
	if (error)
		goto out;
	ctx->request = token;
	ctx->pending_rx = rx_len;
	ctx->touched = true;
	/* Hardware might have observed any attempted write. */
	ctx->state = CASCADIA_THS_PENDING;
	error = regmap_bulk_write(ctx->map, 0x20, bytes, tx_len);
	if (error)
		goto fault;
	error = ths_lease(ctx);
	if (error)
		goto out;
	error = regmap_write(ctx->map, 0x41, 2 * (rx_len - 1));
	if (error)
		goto fault;
	error = ths_lease(ctx);
	if (error)
		goto out;
	error = regmap_write(ctx->map, 0x40, 2 * tx_len - 1);
	if (error)
		goto fault;
	error = ths_lease(ctx);
	if (!error)
		*request = token;
	goto out;
fault:
	error = ths_unknown(ctx, error);
out:
	mutex_unlock(ctx->owner_lock);
	return error;
}
int cascadia_ths_receive(struct cascadia_ths_idbus *ctx, u64 owner, u64 epoch,
			 u64 request, u8 *reply, size_t capacity, size_t *length)
{
	u8 bytes[CASCADIA_THS_WINDOW];
	unsigned int status;
	size_t n, i;
	int error;

	if (!reply || !length || capacity > CASCADIA_THS_WINDOW ||
	    (ctx && (ths_overlap(reply, capacity, ctx, sizeof(*ctx)) ||
		     ths_overlap(length, sizeof(*length), ctx, sizeof(*ctx)))) ||
	    ths_overlap(reply, capacity, length, sizeof(*length)))
		return -EINVAL;
	*length = 0;
	memset(reply, 0, capacity);
	error = ths_identity(ctx, owner, epoch);
	if (error)
		return error;
	mutex_lock(ctx->owner_lock);
	error = ths_lease(ctx);
	if (error)
		goto out;
	if (ctx->state != CASCADIA_THS_PENDING || request != ctx->request) {
		error = -ESTALE;
		goto out;
	}
	n = ctx->pending_rx;
	if (!n || n > sizeof(bytes)) {
		error = ths_unknown(ctx, -EPROTO);
		goto out;
	}
	if (capacity < n) {
		error = -ENOSPC;
		goto out;
	}
	error = regmap_read(ctx->map, 0x0d, &status);
	if (error)
		goto fault;
	error = ths_lease(ctx);
	if (error)
		goto out;
	if ((status & 0x38) != 0x10) {
		error = -EAGAIN;
		goto out;
	}
	error = regmap_bulk_read(ctx->map, 0x40 - n, bytes, n);
	if (error)
		goto fault;
	error = ths_lease(ctx);
	if (error)
		goto out;
	for (i = 0; i < n; i++)
		reply[i] = bytes[n - i - 1];
	*length = n;
	ctx->pending_rx = 0;
	ctx->state = CASCADIA_THS_IDLE;
	goto out;
fault:
	error = ths_unknown(ctx, error);
out:
	mutex_unlock(ctx->owner_lock);
	return error;
}
