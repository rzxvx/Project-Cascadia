// SPDX-License-Identifier: GPL-2.0
/* No registration, I/O, cryptography or power/role callback. */
#include <linux/errno.h>
#include <linux/string.h>
#include "cascadia-authcp-aid.h"
static bool aid_overlaps(const void *a, size_t na, const void *b, size_t nb)
{
	unsigned long x = (unsigned long)a, y = (unsigned long)b;
	return x <= y ? y - x < na : x - y < nb;
}
enum aid_stage { AID_NONCE_FIRST, AID_NONCE_SECOND, AID_STATUS,
	AID_READY_FIRST, AID_READY_SECOND, AID_DATA, AID_DONE };
static int aid_fault(struct cascadia_aid_exchange *ctx, int error)
{
	memset(ctx->signature, 0, sizeof(ctx->signature));
	ctx->fault = true;
	ctx->error = error < 0 ? error : -EIO;
	return ctx->error;
}
static int aid_validate(struct cascadia_aid_exchange *ctx, u64 epoch)
{
	if (!ctx || !ctx->initialized)
		return -EINVAL;
	if (ctx->fault)
		return ctx->error;
	if (ctx->epoch != epoch)
		return aid_fault(ctx, -ESTALE);
	if (!ctx->owner_authorized)
		return aid_fault(ctx, -EACCES);
	return 0;
}
int cascadia_aid_init(struct cascadia_aid_exchange *ctx, u64 epoch,
		     bool owner_authorized, u8 version, u16 provider_window,
		     u16 rx_window, const u8 *nonce, size_t nonce_len)
{
	u8 saved_nonce[32];
	const u8 *bytes;
	size_t i;

	if (!ctx || !nonce)
		return -EINVAL;
	if (ctx->initialized)
		return -EALREADY;
	bytes = (const u8 *)ctx;
	for (i = 0; i < sizeof(*ctx); i++)
		if (bytes[i])
			return -EINVAL;
	if (!owner_authorized)
		return -EACCES;
	if (!((version == 2 && nonce_len == 20) ||
	      (version == 3 && nonce_len == 32)) ||
	    rx_window < 3 || rx_window > CASCADIA_AID_RX_MAX ||
	    provider_window < 21 ||
	    (version == 3 && provider_window > 32 && provider_window < 37))
		return -EINVAL;
	memcpy(saved_nonce, nonce, nonce_len);
	memset(ctx, 0, sizeof(*ctx));
	ctx->epoch = epoch;
	ctx->owner_authorized = true;
	ctx->initialized = true;
	ctx->version = version;
	ctx->provider_window = provider_window;
	ctx->rx_window = rx_window;
	ctx->signature_size = version == 3 ? 64 : 128;
	memcpy(ctx->nonce, saved_nonce, nonce_len);
	return 0;
}
int cascadia_aid_plan(struct cascadia_aid_exchange *ctx, u64 epoch,
		     struct cascadia_aid_request *request)
{
	u16 length;
	int error;

	if (!request || (ctx && aid_overlaps(request, sizeof(*request),
					    ctx, sizeof(*ctx))))
		return -EINVAL;
	memset(request, 0, sizeof(*request));
	error = aid_validate(ctx, epoch);
	if (error)
		return error;
	if (ctx->waiting)
		return -EBUSY;
	if (ctx->stage == AID_DONE)
		return -EALREADY;
	switch (ctx->stage) {
	case AID_NONCE_FIRST:
	case AID_NONCE_SECOND:
		request->tx[0] = 0x94;
		request->rx_len = 3;
		if (ctx->version == 2) {
			memcpy(request->tx + 1, ctx->nonce, 20);
			request->tx_len = 21;
		} else {
			length = ctx->provider_window > 32 ? 32 : 16;
			request->tx[2] = ctx->stage == AID_NONCE_SECOND ? 16 : 0;
			request->tx[4] = length;
			memcpy(request->tx + 5, ctx->nonce + request->tx[2], length);
			request->tx_len = length + 5;
		}
		break;
	case AID_STATUS:
		request->tx[0] = 0x96;
		request->tx_len = 1;
		request->rx_len = 2;
		break;
	case AID_READY_FIRST:
	case AID_READY_SECOND:
		request->tx[0] = 0x9a;
		request->tx_len = 1;
		request->rx_len = 2;
		break;
	case AID_DATA:
		length = ctx->signature_size - ctx->offset;
		if (length >= ctx->rx_window)
			length = ctx->rx_window - 1;
		request->tx[0] = 0x98;
		request->tx[1] = ctx->offset >> 8;
		request->tx[2] = ctx->offset;
		request->tx[3] = length >> 8;
		request->tx[4] = length;
		request->tx_len = 5;
		request->rx_len = length + 1;
		ctx->pending_length = length;
		break;
	default:
		return aid_fault(ctx, -EPROTO);
	}
	ctx->waiting = true;
	/* A handed-out request may have reached hardware; never auto-resend it. */
	ctx->touched = true;
	return 0;
}
int cascadia_aid_complete(struct cascadia_aid_exchange *ctx, u64 epoch,
			 int transfer_error, const u8 *reply, size_t reply_len)
{
	u8 saved_reply[CASCADIA_AID_RX_MAX];
	size_t expected;
	int error = aid_validate(ctx, epoch);

	if (error)
		return error;
	if (!ctx->waiting)
		return aid_fault(ctx, -EPROTO);
	if (transfer_error)
		return aid_fault(ctx, transfer_error);
	expected = ctx->stage == AID_DATA ? ctx->pending_length + 1 :
		   (ctx->stage <= AID_NONCE_SECOND ? 3 : 2);
	if (!reply || expected > sizeof(saved_reply) || reply_len != expected)
		return aid_fault(ctx, -EPROTO);
	memcpy(saved_reply, reply, reply_len);
	reply = saved_reply;
	switch (ctx->stage) {
	case AID_NONCE_FIRST:
	case AID_NONCE_SECOND:
		/* Native final nonce ACK requires signed byte1 < 0. */
		if (reply[0] != 0x95 || !(reply[1] & 0x80))
			return aid_fault(ctx, -EPROTO);
		if (ctx->stage == AID_NONCE_FIRST && ctx->version == 3 &&
		    ctx->provider_window <= 32)
			ctx->stage = AID_NONCE_SECOND;
		else
			ctx->stage = AID_STATUS;
		break;
	case AID_STATUS:
		if (reply[0] != 0x97)
			return aid_fault(ctx, -EPROTO);
		if ((reply[1] & 0xc0) != 0xc0)
			return aid_fault(ctx, -EBUSY);
		ctx->stage = AID_READY_FIRST;
		break;
	case AID_READY_FIRST:
	case AID_READY_SECOND:
		if (reply[0] != 0x9b || reply[1])
			return aid_fault(ctx, -EPROTO);
		ctx->stage = ctx->stage == AID_READY_FIRST ?
			     AID_READY_SECOND : AID_DATA;
		break;
	case AID_DATA:
		if (reply[0] != 0x99 || !ctx->pending_length ||
		    ctx->offset + ctx->pending_length > ctx->signature_size)
			return aid_fault(ctx, -EPROTO);
		memcpy(ctx->signature + ctx->offset, reply + 1,
		       ctx->pending_length);
		ctx->offset += ctx->pending_length;
		if (ctx->offset == ctx->signature_size)
			ctx->stage = AID_DONE;
		break;
	default:
		return aid_fault(ctx, -EPROTO);
	}
	ctx->waiting = false;
	return 0;
}
int cascadia_aid_copy_signature(struct cascadia_aid_exchange *ctx, u64 epoch,
			       u8 *signature, size_t capacity, size_t *length)
{
	int error;

	if (!signature || !length || capacity > CASCADIA_AID_SIGNATURE_MAX ||
	    (ctx && (aid_overlaps(signature, capacity, ctx, sizeof(*ctx)) ||
		     aid_overlaps(length, sizeof(*length), ctx, sizeof(*ctx)))) ||
	    aid_overlaps(signature, capacity, length, sizeof(*length)))
		return -EINVAL;
	*length = 0;
	memset(signature, 0, capacity);
	error = aid_validate(ctx, epoch);
	if (error)
		return error;
	if (ctx->stage != AID_DONE || ctx->waiting)
		return -EAGAIN;
	if (capacity < ctx->signature_size)
		return -ENOSPC;
	memcpy(signature, ctx->signature, ctx->signature_size);
	*length = ctx->signature_size;
	return 0;
}
