/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CASCADIA_ACCESSORY_ID_H
#define CASCADIA_ACCESSORY_ID_H

#include <linux/types.h>

/* Read-only semantic input. These fields grant no hardware authority. */
struct cascadia_id_policy {
	bool debugger_known;
	bool debugger_allowed;
	bool source_ignore_vbus_known;
	bool source_ignore_vbus_present;
	bool source_md_root_known;
	bool source_md_root;
};

struct cascadia_id_record {
	/* Native parser validity only; not identity, role or power admission. */
	bool valid;
	u32 pair[2];
	u32 usb_control_low_flags;
};

/* Pure decoder: no transport, rail, GPIO, authentication or role publication. */
int cascadia_ths_id_frame(const u8 *frame, size_t length, u8 status,
			 u8 id[6]);
int cascadia_accessory_id_decode(const u8 *id, size_t length,
				const struct cascadia_id_policy *policy,
				struct cascadia_id_record *record);

#endif
