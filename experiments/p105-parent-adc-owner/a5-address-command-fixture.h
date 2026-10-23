/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2020 Corellium LLC. Unchanged actual address-command body. */
static void apple_s5l8940x_i2c_start_write(struct apple_s5l8940x_i2c *i2c)
{
    struct i2c_msg *msg = i2c->msg;
    unsigned max = MTXFIFO_CHUNK;
    unsigned endflag = 0;

    if(!(msg->flags & I2C_M_NOSTART)) {
        endflag = (!msg->len && i2c->last) ? REG_MTXFIFO_STOP : 0;
        writel(REG_MTXFIFO_START | endflag | (msg->addr << 1), i2c->base + REG_MTXFIFO);
        max --;
    }

    apple_s5l8940x_i2c_continue_write(i2c, max);

    writel(REG_SMSTA_MTN | REG_SMSTA_XEN | REG_SMSTA_ERROR, i2c->base + REG_IRQMASK);
}
