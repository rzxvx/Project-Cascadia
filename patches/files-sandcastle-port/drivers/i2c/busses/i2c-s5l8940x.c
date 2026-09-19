/*
 * I2C bus driver for S5L8940X (Apple A5) SoCs
 *
 * Copyright (C) 2020 Corellium LLC
 *
 * Based on:
 * Copyright (C) 2015 Paradox Innovation Ltd.
 *
 * Controller is definitely derived from PASemi I2C,
 * but that driver is polling and we can do better.
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/consumer.h>

#define REG_MTXFIFO             0x00
#define  REG_MTXFIFO_READ       0x00000400
#define  REG_MTXFIFO_STOP       0x00000200
#define  REG_MTXFIFO_START      0x00000100
#define  REG_MTXFIFO_DATA_MASK  0x000000ff
#define REG_MRXFIFO             0x04
#define  REG_MRXFIFO_EMPTY      0x00000100
#define  REG_MRXFIFO_DATA_MASK  0x000000ff
#define REG_FIFOLVL             0x08
#define  REG_FIFOLVL_RX_SHIFT   8
#define  REG_FIFOLVL_RX_MASK    0x0000ff00
#define REG_RESET               0x10
#define  REG_RESET_BIT          0x80000000
#define REG_SMSTA               0x14
#define  REG_SMSTA_XEN          0x08000000
#define  REG_SMSTA_ERROR        0x00800040
#define  REG_SMSTA_MTN          0x00200000
#define  REG_SMSTA_RXDONE       0x00100000
#define REG_CTL                 0x1c
#define  REG_CTL_REV6_UNK       0x00000800
#define  REG_CTL_MRR            0x00000400
#define  REG_CTL_MTR            0x00000200
#define  REG_CTL_CLKDIV_MASK    0x000000ff
#define REG_IRQMASK             0x18
#define REG_REV                 0x28
#define REG_RDCOUNT             0x2c
#define  REG_RDCOUNT_SHIFT      8
#define  REG_RDCOUNT_MASK       0x0000ff00
#define REG_TBUF                0x30
#define REG_FILTER              0x38
#define  REG_FILTER_DFLT        0x00000200
#define REG_LOCK                0x44

#define DEFAULT_FREQ            100000
#define TIMEOUT_MS              100
#define MTXFIFO_CHUNK           16

struct apple_s5l8940x_i2c {
    struct i2c_adapter adap;
    struct device *dev;
    void __iomem *base;
    struct clk *clk;
    unsigned int frequency;
    struct pinctrl *pctrl;

    unsigned int hw_rev;
    unsigned int clkdiv;

    struct i2c_msg *msg;
    unsigned int compl_ptr, tx_ptr;
    int last;
    spinlock_t lock;
    struct mutex mtx;
    struct completion done;
    int error;
};

static void apple_s5l8940x_i2c_continue_write(struct apple_s5l8940x_i2c *i2c, unsigned max)
{
    struct i2c_msg *msg = i2c->msg;
    unsigned endflag = 0;

    while(i2c->tx_ptr < msg->len) {
        if(!max)
            break;
        if(i2c->tx_ptr + 1 == msg->len && i2c->last)
            endflag = REG_MTXFIFO_STOP;
        writel(endflag | msg->buf[i2c->tx_ptr], i2c->base + REG_MTXFIFO);
        i2c->tx_ptr ++;
        max --;
    }
}

static void apple_s5l8940x_i2c_continue_read(struct apple_s5l8940x_i2c *i2c)
{
    struct i2c_msg *msg = i2c->msg;
    unsigned num, rxdata;

    num = (readl(i2c->base + REG_FIFOLVL) & REG_FIFOLVL_RX_MASK) >> REG_FIFOLVL_RX_SHIFT;
    while(num --) {
        rxdata = readl(i2c->base + REG_MRXFIFO);
        if(i2c->compl_ptr < msg->len) {
            msg->buf[i2c->compl_ptr] = rxdata;
            i2c->compl_ptr ++;
        }
    }
}

static irqreturn_t apple_s5l8940x_i2c_irq(int irq, void *dev_id)
{
    struct apple_s5l8940x_i2c *i2c = dev_id;
    struct i2c_msg *msg = i2c->msg;
    unsigned long flags;
    unsigned status;

    spin_lock_irqsave(&i2c->lock, flags);

    status = readl(i2c->base + REG_SMSTA);
    writel(0, i2c->base + REG_IRQMASK);
    writel(status, i2c->base + REG_SMSTA);

    if(!msg) {
        i2c->error = -EINVAL;
    } else if(status & REG_SMSTA_ERROR) {
        i2c->error = -EIO;
        complete(&i2c->done);
    } else if(status & REG_SMSTA_MTN) {
        i2c->error = -ENXIO;
        complete(&i2c->done);
    } else if(msg->flags & I2C_M_RD) {
        apple_s5l8940x_i2c_continue_read(i2c);
        if(i2c->compl_ptr < msg->len)
            writel(REG_SMSTA_MTN | REG_SMSTA_RXDONE | REG_SMSTA_ERROR, i2c->base + REG_IRQMASK);
        else
            complete(&i2c->done);
    } else {
        if(status & REG_SMSTA_XEN) {
            i2c->compl_ptr = i2c->tx_ptr;
            if(i2c->tx_ptr < msg->len) {
                writel(1, i2c->base + REG_LOCK);
                apple_s5l8940x_i2c_continue_write(i2c, MTXFIFO_CHUNK);
                writel(0, i2c->base + REG_LOCK);
            }
        }
        if(i2c->compl_ptr < msg->len)
            writel(REG_SMSTA_MTN | REG_SMSTA_XEN | REG_SMSTA_ERROR, i2c->base + REG_IRQMASK);
        else
            complete(&i2c->done);
    }

    spin_unlock_irqrestore(&i2c->lock, flags);

    return IRQ_HANDLED;
}

static void apple_s5l8940x_i2c_start_read(struct apple_s5l8940x_i2c *i2c)
{
    struct i2c_msg *msg = i2c->msg;

    writel((readl(i2c->base + REG_RDCOUNT) & ~REG_RDCOUNT_MASK) | (msg->len << REG_RDCOUNT_SHIFT), i2c->base + REG_RDCOUNT);

    if(!(msg->flags & I2C_M_NOSTART))
        writel(REG_MTXFIFO_START | (msg->addr << 1) | 1, i2c->base + REG_MTXFIFO);
    writel(REG_MTXFIFO_READ | (i2c->last ? REG_MTXFIFO_STOP : 0) | msg->len, i2c->base + REG_MTXFIFO);

    writel(REG_SMSTA_MTN | REG_SMSTA_RXDONE | REG_SMSTA_ERROR, i2c->base + REG_IRQMASK);
}

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

static int apple_s5l8940x_i2c_xfer_msg(struct apple_s5l8940x_i2c *i2c, struct i2c_msg *msg, int first, int last)
{
    unsigned long timeout = msecs_to_jiffies(TIMEOUT_MS);
    unsigned long flags;

    spin_lock_irqsave(&i2c->lock, flags);
    i2c->msg = msg;
    i2c->compl_ptr = 0;
    i2c->tx_ptr = 0;
    i2c->last = 1; /* Hx controller doesn't seem to like leaving a transaction hanging */
    i2c->error = 0;

    reinit_completion(&i2c->done);

    writel(1, i2c->base + REG_LOCK);

    if(msg->flags & I2C_M_RD)
        apple_s5l8940x_i2c_start_read(i2c);
    else
        apple_s5l8940x_i2c_start_write(i2c);

    writel(0, i2c->base + REG_LOCK);

    spin_unlock_irqrestore(&i2c->lock, flags);

    /* This controller's interrupt (hwirq 10) never reaches us -- the AIC
     * masks its lines after rearm -- so the transfer is driven by polling and
     * we call the handler ourselves whenever the controller has something to
     * service.
     *
     * The condition used to be MTN | RXDONE | ERROR, leaving out XEN.  XEN is
     * exactly the bit a write completes on, so every write to the PMU ran the
     * full 100 ms and was then reported as a timeout with SMSTA = 0x08010118
     * -- XEN set, sitting there ignored.  Wait on XEN as well, and let the
     * handler decide when the message is actually finished: a write longer
     * than one FIFO chunk needs several rounds before it completes. */
    {
        unsigned int poll_us = TIMEOUT_MS * 1000;
        u32 smsta;

        timeout = 0;
        while(poll_us) {
            smsta = readl(i2c->base + REG_SMSTA);
            if(smsta & (REG_SMSTA_XEN | REG_SMSTA_MTN | REG_SMSTA_RXDONE | REG_SMSTA_ERROR))
                apple_s5l8940x_i2c_irq(0, i2c);
            if(try_wait_for_completion(&i2c->done)) {
                timeout = 1;
                break;
            }
            udelay(10);
            poll_us -= 10;
        }
        if(!timeout)
            dev_err(i2c->dev, "transfer timed out, SMSTA=%#x\n",
                    readl(i2c->base + REG_SMSTA));
    }

    if(timeout == 0) {
        spin_lock_irqsave(&i2c->lock, flags);
        writel(REG_SMSTA_XEN | REG_SMSTA_MTN | REG_SMSTA_ERROR, i2c->base + REG_SMSTA);
        writel(0, i2c->base + REG_IRQMASK);
        writel(REG_RESET_BIT, i2c->base + REG_RESET);
        spin_unlock_irqrestore(&i2c->lock, flags);
        return -ETIMEDOUT;
    }

    return i2c->error;
}

static int apple_s5l8940x_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
    struct apple_s5l8940x_i2c *i2c = adap->algo_data;
    int i, ret;

    mutex_lock(&i2c->mtx);

    writel(i2c->clkdiv | (i2c->hw_rev >= 6 ? REG_CTL_REV6_UNK : 0), i2c->base + REG_CTL);
    writel(REG_SMSTA_XEN | REG_SMSTA_MTN | REG_SMSTA_ERROR, i2c->base + REG_SMSTA);
    writel(0, i2c->base + REG_IRQMASK);
    writel(REG_RESET_BIT, i2c->base + REG_RESET);
    writel(readl(i2c->base + REG_FILTER) | REG_FILTER_DFLT, i2c->base + REG_FILTER);

    for(i=0; i<num; i++) {
        ret = apple_s5l8940x_i2c_xfer_msg(i2c, &msgs[i], i == 0, i == num - 1);
        if(ret) {
            /* this used to return with the mutex still held, so the very next
             * transfer blocked on mutex_lock for good and the boot stopped
             * dead right after the first failed PMU access */
            mutex_unlock(&i2c->mtx);
            return ret;
        }
    }

    mutex_unlock(&i2c->mtx);

    return num;
}

static int apple_s5l8940x_i2c_init_hw(struct apple_s5l8940x_i2c *i2c)
{
    unsigned long clk_rate = clk_get_rate(i2c->clk);
    unsigned int clkdiv;

    /* The pinmux hack that used to live here forced pins 4 and 5, which are
     * not this bus: the ADT puts I2C0 on 0x0604 and 0x0605, linear pins 52
     * and 53, and a dump of the pad registers shows iBoot already leaves both
     * muxed to the peripheral (PERIPH set, 0x621).  Writing pins 4,5 only
     * clobbered two unrelated pads. */

    clkdiv = DIV_ROUND_UP(clk_rate, 16 * i2c->frequency);
    if(clkdiv < 4)
        clkdiv = 4;
    if(clkdiv > 255) {
        dev_err(i2c->dev, "can't set bus speed of %u Hz\n", i2c->frequency);
        return -EINVAL;
    }

    i2c->hw_rev = readl(i2c->base + REG_REV);
    i2c->clkdiv = clkdiv;
    dev_err(i2c->dev, "Hx I2C core rev %d, clkdiv %d.\n", i2c->hw_rev, i2c->clkdiv);

    return 0;
}

static u32 apple_s5l8940x_i2c_func(struct i2c_adapter *adap)
{
    return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm apple_s5l8940x_i2c_algorithm = {
    .master_xfer    = apple_s5l8940x_i2c_xfer,
    .functionality  = apple_s5l8940x_i2c_func,
};

static int apple_s5l8940x_i2c_probe(struct platform_device *pdev)
{
    struct device_node *np = pdev->dev.of_node;
    struct apple_s5l8940x_i2c *i2c;
    struct resource *r;
    int ret = 0, irq;

    i2c = devm_kzalloc(&pdev->dev, sizeof(struct apple_s5l8940x_i2c), GFP_KERNEL);
    if(!i2c)
        return -ENOMEM;

    if(of_property_read_u32(pdev->dev.of_node, "clock-frequency", &i2c->frequency))
        i2c->frequency = DEFAULT_FREQ;

    i2c->dev = &pdev->dev;
    platform_set_drvdata(pdev, i2c);

    spin_lock_init(&i2c->lock);
    mutex_init(&i2c->mtx);
    init_completion(&i2c->done);

    i2c->clk = devm_clk_get(&pdev->dev, NULL);
    if(IS_ERR(i2c->clk))
        return PTR_ERR(i2c->clk);

    r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    i2c->base = devm_ioremap_resource(&pdev->dev, r);
    if(IS_ERR(i2c->base))
        return PTR_ERR(i2c->base);

    irq = platform_get_irq(pdev, 0);
    if(irq < 0)
        return irq;

    ret = devm_request_irq(&pdev->dev, irq, apple_s5l8940x_i2c_irq, 0, dev_name(&pdev->dev), i2c);
    if(ret < 0)
        return ret;

    strscpy(i2c->adap.name, "Hx I2C adapter", sizeof(i2c->adap.name));
    i2c->adap.owner = THIS_MODULE;
    i2c->adap.algo = &apple_s5l8940x_i2c_algorithm;
    i2c->adap.dev.parent = &pdev->dev;
    i2c->adap.dev.of_node = np;
    i2c->adap.algo_data = i2c;

    ret = apple_s5l8940x_i2c_init_hw(i2c);
    if(ret)
        return ret;

    ret = clk_prepare_enable(i2c->clk);
    if(ret < 0)
        return ret;

    i2c->pctrl = devm_pinctrl_get_select_default(i2c->dev);

    ret = i2c_add_adapter(&i2c->adap);
    if(ret < 0) {
        clk_disable_unprepare(i2c->clk);
        return ret;
    }

    return 0;
}

static void apple_s5l8940x_i2c_remove(struct platform_device *pdev)
{
    struct apple_s5l8940x_i2c *i2c = platform_get_drvdata(pdev);

    i2c_del_adapter(&i2c->adap);
    clk_disable_unprepare(i2c->clk);
}

static const struct of_device_id apple_s5l8940x_i2c_match[] = {
    { .compatible = "apple,s5l8940x-i2c" },
    { },
};
MODULE_DEVICE_TABLE(of, apple_s5l8940x_i2c_match);

static struct platform_driver apple_s5l8940x_i2c_driver = {
    .probe   = apple_s5l8940x_i2c_probe,
    .remove  = apple_s5l8940x_i2c_remove,
    .driver  = {
        .name  = "hx-i2c",
        .of_match_table = apple_s5l8940x_i2c_match,
    },
};
module_platform_driver(apple_s5l8940x_i2c_driver);

MODULE_DESCRIPTION("S5L8940X (Apple A5) SoC I2C master driver");
MODULE_LICENSE("GPL v2");
