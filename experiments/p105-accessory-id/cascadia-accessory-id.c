// SPDX-License-Identifier: GPL-2.0
/*
 * Source-only 13G36 semantic decoder. No driver registration or I/O.
 * A valid record is not an adapter identity, authentication or power grant.
 */
#include <linux/errno.h>
#include <linux/string.h>
#include "cascadia-accessory-id.h"

#define ID_USB0	0x75736230U
#define ID_USB1	0x75736231U
#define ID_USBC	0x75736263U
#define ID_URT0	0x75727430U
#define ID_URT1	0x75727431U
#define ID_URT2	0x75727432U
#define ID_MBUS	0x6d627573U
#define ID_JTAG	0x6a746167U

int cascadia_ths_id_frame(const u8 *frame, size_t length, u8 status, u8 id[6])
{
	u8 response[7] = { 0 };
	int i;

	if (!id)
		return -EINVAL;
	if (frame && length == 7)
		memcpy(response, frame, 7);
	memset(id, 0, 6);
	if (!frame || length != 7)
		return -EINVAL;
	if (!(status & 0x80))
		return -EAGAIN;
	if (response[6] != 0x75)
		return -EPROTO;
	for (i = 0; i < 6; i++)
		id[i] = response[5 - i];
	return 0;
}

int cascadia_accessory_id_decode(const u8 *id, size_t length,
				const struct cascadia_id_policy *policy,
				struct cascadia_id_record *record)
{
	u8 raw[6] = { 0 };
	struct cascadia_id_policy captured_policy = { 0 };
	u32 pair[2] = { 0, 0 };
	u32 low, usb, mbus, usbc, uart, count, selector, flags;
	bool valid = true;

	if (!record)
		return -EINVAL;
	if (id && length == 6)
		memcpy(raw, id, 6);
	if (policy)
		memcpy(&captured_policy, policy, sizeof(captured_policy));
	memset(record, 0, sizeof(*record));
	if (!id || length != 6 || !policy)
		return -EINVAL;
	id = raw;
	policy = &captured_policy;
	low = id[0] & 3;
	if (!(id[0] & 0x20)) {
		usb = id[0] & 0x18;
		mbus = (id[0] >> 2) & 1;
		usbc = (id[2] >> 3) & 1;
		uart = !!low;
		count = uart + !!usb + mbus + usbc;
		if (mbus && usb && low == 1 && (id[3] & 0x70) == 0x20) {
			count -= 2;
			mbus = 0;
			uart = 0;
		}
		if (count >= 3)
			return -EPROTO;
		if (usb)
			pair[0] = ID_USB0;
		if (uart)
			pair[!!usb] = ID_URT0;
		if (mbus)
			pair[(!!usb) | uart] = ID_MBUS;
		if (usbc)
			pair[(!!usb) | uart | mbus] = ID_USBC;
	} else {
		selector = (id[0] >> 2) & 7;
		count = 1;
		if (selector == 3) {
			pair[0] = ID_JTAG;
		} else if (selector == 2) {
			if (!policy->debugger_known)
				return -EACCES;
			if (policy->debugger_allowed)
				pair[0] = ID_USB1;
		} else if (!selector) {
			pair[0] = ID_USB0;
		} else {
			count = 0;
		}
		if (low == 2) {
			if (pair[0] == ID_USB0)
				valid = false;
			else
				pair[1] = ID_USB0;
		} else if (low == 1) {
			if (!policy->debugger_known)
				return -EACCES;
			if (policy->debugger_allowed)
				pair[1] = ID_URT2;
		} else if (!low) {
			pair[1] = ID_URT1;
		} else {
			valid = false;
		}
		valid = valid && count;
	}
	if (!valid)
		return -EPROTO;
	flags = 1;
	if (pair[0] == ID_USB0 || pair[1] == ID_USB0) {
		if (!(id[0] & 0x20) && ((id[0] >> 3) & 3) != 2) {
			if (((id[0] >> 3) & 3) == 1) {
				flags = 5;
			} else {
				if (!policy->source_md_root_known)
					return -EACCES;
				flags = policy->source_md_root ? 2 : 1;
			}
		} else if (id[2] & 0x20) {
			flags = 6;
		} else {
			if (!policy->source_ignore_vbus_known)
				return -EACCES;
			flags = policy->source_ignore_vbus_present == 1 ? 4 : 2;
		}
	}
	record->pair[0] = pair[0];
	record->pair[1] = pair[1];
	record->usb_control_low_flags = flags;
	record->valid = true;
	return 0;
}
