/* SPDX-License-Identifier: GPL-2.0 */
#ifndef D1946_CURRENT_CORE_H
#define D1946_CURRENT_CORE_H
#ifdef CASCADIA_OFFLINE
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
typedef uint8_t u8;
#else
#include <linux/types.h>
#include <linux/errno.h>
#endif
/* Caller serializes the entire transaction, including all readbacks. */
struct d1946_io {
	void *context;
	int (*read)(void *, unsigned int, u8 *, unsigned int);
	int (*write)(void *, unsigned int, const u8 *, unsigned int);
};
int d1946_update_checked(const struct d1946_io *, unsigned int, u8, u8);
int d1946_charge_request(unsigned int ma);
int d1946_current_limit(const struct d1946_io *, unsigned int, u8);
int d1946_input_limit(const struct d1946_io *, unsigned int);
int d1946_input_handoff(const struct d1946_io *);
int d1946_input_profile(unsigned int);
int d1946_mask_checked(const struct d1946_io *, unsigned int, bool, u8 *);
#endif
