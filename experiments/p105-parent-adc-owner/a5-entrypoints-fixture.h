/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2020 Corellium LLC. */
/* Corellium original wrappers, with this private raw_admit extension. */
static int apple_s5l8940x_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
    struct apple_s5l8940x_i2c *i2c = adap->algo_data;
    int ret;

    ret = apple_s5l8940x_i2c_raw_admit(i2c, msgs, num);
    if (ret)
        return ret;

    /* An early version returned from inside the transfer with this still
     * held, so the very next transfer blocked for good and the boot stopped
     * dead right after the first failed PMU access: one lock, one unlock. */
    mutex_lock(&i2c->mtx);
    ret = apple_s5l8940x_i2c_do_xfer(i2c, msgs, num);
    mutex_unlock(&i2c->mtx);
    return ret;
}
static int apple_s5l8940x_i2c_xfer_atomic(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
    struct apple_s5l8940x_i2c *i2c = adap->algo_data;
    int ret = apple_s5l8940x_i2c_raw_admit(i2c, msgs, num);

    if (ret)
        return ret;
    return apple_s5l8940x_i2c_do_xfer(i2c, msgs, num);
}
