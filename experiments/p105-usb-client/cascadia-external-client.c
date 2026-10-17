/* SPDX-License-Identifier: GPL-2.0 */
/* Checked client lifetime from actual P105 port callbacks; no hardware backend. */
#include <linux/errno.h>
#include <linux/string.h>
#include "cascadia-external-client.h"

#define CASCADIA_STANDBY_CLOCK_MASK 0xfff9ff8fU

int cascadia_external_client_init(struct cascadia_external_client *client,
	u32 raw_clock_mask)
{
	if (!client)
		return -EINVAL;
	if (client->initialized)
		return -EALREADY;
	memset(client, 0, sizeof(*client));
	client->initialized = true;
	client->raw_clock_mask = raw_clock_mask;
	client->phy_known = true;
	client->clock_known = true;
	return 0;
}

static int validate(struct cascadia_external_client *client, u64 epoch)
{
	int error;

	if (!client->initialized)
		return -EINVAL;
	if (!client->permitted)
		return -EACCES;
	if (!client->ops || !client->ops->validate_epoch ||
	    !client->ops->parent_clock_mask || !client->ops->phy_client_state)
		return -ENODEV;
	if (epoch != client->connection_epoch)
		return -ESTALE;
	error = client->ops->validate_epoch(client->context, epoch);
	return error > 0 ? -ESTALE : error;
}

static int checked_epoch(struct cascadia_external_client *client, u64 epoch)
{
	int error = validate(client, epoch);

	if (error && client->touched)
		client->fault = true;
	return error;
}

static int clock_update(struct cascadia_external_client *client, u64 epoch,
	u32 mask)
{
	int error = checked_epoch(client, epoch);

	if (error)
		return error;
	error = client->ops->parent_clock_mask(client->context, epoch, mask);

	client->touched = true;
	if (!error)
		error = checked_epoch(client, epoch);
	if (error) {
		client->clock_known = false;
		client->fault = true;
		return error < 0 ? error : -EIO;
	}
	client->parent_mask = mask;
	client->clock_known = true;
	return 0;
}

static int phy_update(struct cascadia_external_client *client, u64 epoch,
	enum cascadia_external_client_state state)
{
	unsigned int request;
	int error;

	switch (state) {
	case CASCADIA_CLIENT_ACTIVE:
		request = 5;
		break;
	case CASCADIA_CLIENT_STANDBY:
		request = 25;
		break;
	case CASCADIA_CLIENT_OFF:
		request = 10;
		break;
	default:
		return -EINVAL;
	}
	error = checked_epoch(client, epoch);
	if (error)
		return error;
	error = client->ops->phy_client_state(client->context, epoch, request);
	client->touched = true;
	if (!error)
		error = checked_epoch(client, epoch);
	if (error) {
		client->phy_known = false;
		client->fault = true;
		return error < 0 ? error : -EIO;
	}
	client->phy_state = state;
	client->phy_known = true;
	return 0;
}

int cascadia_external_client_prepare(struct cascadia_external_client *client,
	enum cascadia_external_client_state state, u64 epoch)
{
	int error;

	if (!client || state < CASCADIA_CLIENT_OFF || state > CASCADIA_CLIENT_STANDBY)
		return -EINVAL;
	error = checked_epoch(client, epoch);
	if (error)
		return error;
	if (client->fault)
		return -EIO;
	if (client->pending)
		return -EBUSY;
	client->pending = true;
	client->target = state;
	error = clock_update(client, epoch, client->raw_clock_mask);
	if (error)
		return error;
	if (state == CASCADIA_CLIENT_ACTIVE) {
		error = phy_update(client, epoch, state);
		if (error)
			return error;
	}
	return checked_epoch(client, epoch);
}

int cascadia_external_client_complete(struct cascadia_external_client *client,
	u64 epoch, int controller_error)
{
	int error;

	if (!client)
		return -EINVAL;
	error = checked_epoch(client, epoch);
	if (error)
		return error;
	if (!client->pending)
		return -EINVAL;
	if (controller_error) {
		client->fault = true;
		return controller_error < 0 ? controller_error : -EIO;
	}
	if (client->fault)
		return -EIO;
	if (client->target == CASCADIA_CLIENT_OFF)
		return cascadia_external_client_cleanup(client, epoch, true);
	if (client->target == CASCADIA_CLIENT_STANDBY) {
		error = phy_update(client, epoch, CASCADIA_CLIENT_STANDBY);
		if (error)
			return error;
		error = clock_update(client, epoch,
			client->raw_clock_mask & CASCADIA_STANDBY_CLOCK_MASK);
		if (error)
			return error;
	}
	error = checked_epoch(client, epoch);
	if (error)
		return error;
	client->pending = false;
	return 0;
}

int cascadia_external_client_cleanup(struct cascadia_external_client *client,
	u64 epoch, bool controller_quiesced)
{
	int error;

	if (!client)
		return -EINVAL;
	error = checked_epoch(client, epoch);
	if (error)
		return error;
	if (!controller_quiesced)
		return -EBUSY;
	if (!client->touched) {
		client->pending = false;
		return 0;
	}
	if (!client->phy_known || client->phy_state != CASCADIA_CLIENT_OFF) {
		error = phy_update(client, epoch, CASCADIA_CLIENT_OFF);
		if (error)
			return error;
	}
	if (!client->clock_known || client->parent_mask) {
		error = clock_update(client, epoch, 0);
		if (error)
			return error;
	}
	error = checked_epoch(client, epoch);
	if (error)
		return error;
	client->fault = false;
	client->touched = false;
	client->pending = false;
	return 0;
}
