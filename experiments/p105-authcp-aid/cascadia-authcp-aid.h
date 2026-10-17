/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CASCADIA_AUTHCP_AID_H
#define CASCADIA_AUTHCP_AID_H
#include <linux/types.h>
#define CASCADIA_AID_TX_MAX 37
#define CASCADIA_AID_RX_MAX 129
#define CASCADIA_AID_SIGNATURE_MAX 128
struct cascadia_aid_request {
	u8 tx[CASCADIA_AID_TX_MAX];
	u16 tx_len;
	u16 rx_len;
};
struct cascadia_aid_exchange {
	u64 epoch;
	u8 nonce[32];
	u8 signature[CASCADIA_AID_SIGNATURE_MAX];
	u16 provider_window;
	u16 rx_window;
	u16 signature_size;
	u16 offset;
	u16 pending_length;
	u8 version;
	u8 stage;
	bool initialized;
	bool waiting;
	bool touched;
	bool fault;
	bool owner_authorized;
	int error;
};
/* ctx must be zero-initialized fresh storage; reinitialization is refused. */
int cascadia_aid_init(struct cascadia_aid_exchange *ctx, u64 epoch,
		     bool owner_authorized, u8 version, u16 provider_window,
		     u16 rx_window, const u8 *nonce, size_t nonce_len);
/* Live valid non-error pointers, immutable initialized config, serialization,
 * never-reused epochs and exact IDbus ownership are caller obligations. No I/O. */
int cascadia_aid_plan(struct cascadia_aid_exchange *ctx, u64 epoch,
		     struct cascadia_aid_request *request);
int cascadia_aid_complete(struct cascadia_aid_exchange *ctx, u64 epoch,
			 int transfer_error, const u8 *reply, size_t reply_len);
/* Returns unverified signature bytes. Never an authentication/role grant. */
int cascadia_aid_copy_signature(struct cascadia_aid_exchange *ctx, u64 epoch,
			       u8 *signature, size_t capacity, size_t *length);
#endif
