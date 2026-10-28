/* SPDX-License-Identifier: GPL-2.0-only */
/* Generic algorithm path under existing adapter-lock contract. */
static int apple_s5l8940x_i2c_raw_admit(struct apple_s5l8940x_i2c *i2c,
				     struct i2c_msg *msgs, int num)
{
	int i;

	lockdep_assert_held(&i2c->adap.bus_lock);
	if (!i2c->pmu_owner)
		return 0;
	for (i = 0; i < num; i++) {
		/* Controller shifts into the FIFO command word without masking:
		 * an unsupported high address can alias the reserved 7-bit PMU. */
		if (msgs[i].addr > 0x7f || (msgs[i].flags & I2C_M_TEN))
			return -EINVAL;
		if (msgs[i].addr == 0x3c)
			return -EBUSY;
	}
	return 0;
}
