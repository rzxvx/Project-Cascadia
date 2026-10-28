/* SPDX-License-Identifier: GPL-2.0 */
/* Offline-reviewed transport component; no driver registration or I/O. */
#include <linux/errno.h>
#include "cascadia-accessory-role.h"

static u16 role_get_le16(const u8 *bytes)
{
	return (u16)bytes[0] | ((u16)bytes[1] << 8);
}

int cascadia_accessory_role_setup(const u8 *setup, size_t setup_length,
	const struct cascadia_accessory_role_policy *policy, u8 response[4])
{
	u16 value, index, length;
	int error;

	if (!setup || !policy || setup_length != 8)
		return -EINVAL;
	value = role_get_le16(setup + 2);
	index = role_get_le16(setup + 4);
	length = role_get_le16(setup + 6);

	if (setup[0] == 0xc0 && setup[1] == 0x53 && !value && !index && length == 4) {
		if (!response)
			return -EINVAL;
		response[0] = policy->value_mask & 0xff;
		response[1] = policy->value_mask >> 8;
		response[2] = policy->index_mask & 0xff;
		response[3] = policy->index_mask >> 8;
		return 4;
	}
	if (setup[0] != 0x40 || setup[1] != 0x51 || length)
		return -EOPNOTSUPP;
	if ((value & ~policy->value_mask) || (index & ~policy->index_mask))
		return -EINVAL;

	/* An intentional conservative addition to the firmware's mask-only check. */
	if (!policy->allow_switch || !policy->authorize || !policy->request_switch)
		return -EACCES;
	error = policy->authorize(policy->context, policy->connection_epoch,
				  value, index);
	if (error)
		return error < 0 ? error : -EACCES;
	error = policy->request_switch(policy->context, policy->connection_epoch, setup);
	return error > 0 ? -EIO : error;
}
