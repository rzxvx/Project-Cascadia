/* SPDX-License-Identifier: GPL-2.0-only */
/* Included in actual A5 driver. Every entry requires adapter segment lock.
 * No generic core retries, mutable timeout policy, or adapter proxy copy. */
static struct apple_s5l8940x_i2c *pmu_controller(struct i2c_client *client)
{
	struct i2c_adapter *adap;

	if (IS_ERR_OR_NULL(client) || IS_ERR_OR_NULL(client->adapter) ||
	    client->addr != 0x3c || client->flags)
		return ERR_PTR(-EINVAL);
	adap = client->adapter;
	if (adap->algo != &apple_s5l8940x_i2c_algorithm ||
	    IS_ERR_OR_NULL(adap->algo_data) || adap->quirks)
		return ERR_PTR(-EOPNOTSUPP);
	lockdep_assert_held(&adap->bus_lock);
	return adap->algo_data;
}

int apple_s5l8940x_i2c_pmu_reserve(struct i2c_client *client)
{
	struct apple_s5l8940x_i2c *i2c = pmu_controller(client);

	if (IS_ERR(i2c))
		return PTR_ERR(i2c);
	if (i2c->pmu_owner)
		return -EBUSY;
	if (test_bit(I2C_ALF_IS_SUSPENDED, &client->adapter->locked_flags))
		return -ESHUTDOWN;
	i2c->pmu_owner = client;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_s5l8940x_i2c_pmu_reserve);

int apple_s5l8940x_i2c_pmu_release(struct i2c_client *client)
{
	struct apple_s5l8940x_i2c *i2c = pmu_controller(client);

	if (IS_ERR(i2c))
		return PTR_ERR(i2c);
	if (i2c->pmu_owner != client)
		return -EPERM;
	i2c->pmu_owner = NULL;
	return 0;
}
EXPORT_SYMBOL_GPL(apple_s5l8940x_i2c_pmu_release);

int apple_s5l8940x_i2c_pmu_once(struct i2c_client *client,
			      struct i2c_msg *msgs, int num, bool atomic)
{
	struct apple_s5l8940x_i2c *i2c = pmu_controller(client);
	struct i2c_adapter *adap;
	int i, ret;

	if (IS_ERR(i2c))
		return PTR_ERR(i2c);
	adap = client->adapter;
	if (i2c->pmu_owner != client)
		return -EPERM;
	if (test_bit(I2C_ALF_IS_SUSPENDED, &adap->locked_flags))
		return -ESHUTDOWN;
	if (IS_ERR_OR_NULL(msgs) || (num != 1 && num != 2))
		return -EINVAL;
	for (i = 0; i < num; i++) {
		if (msgs[i].addr != 0x3c || IS_ERR_OR_NULL(msgs[i].buf) ||
		    !msgs[i].len || msgs[i].len > 257 ||
		    msgs[i].flags != (i == 1 ? I2C_M_RD : 0))
			return -EINVAL;
	}
	if (num == 2 && (msgs[0].len != 1 || msgs[1].len > 256))
		return -EINVAL;
	if (atomic) {
		if (!irqs_disabled() || system_state <= SYSTEM_RUNNING)
			return -EOPNOTSUPP;
		return apple_s5l8940x_i2c_do_xfer(i2c, msgs, num);
	}
	if (irqs_disabled() || in_atomic())
		return -EOPNOTSUPP;
	mutex_lock(&i2c->mtx);
	ret = apple_s5l8940x_i2c_do_xfer(i2c, msgs, num);
	mutex_unlock(&i2c->mtx);
	return ret;
}
EXPORT_SYMBOL_GPL(apple_s5l8940x_i2c_pmu_once);
