/*
 * Driver for multi-touch panels found on S5L8940X (Apple A5) SoC-base mobile devices.
 *
 * Copyright (C) 2020 Corellium LLC
 *
 * Based on: surface3_spi.c
 *   Copyright (c) 2016 Red Hat Inc.
 */

#include <linux/kernel.h>

#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/clk.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/spi/spi.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/math64.h>

struct hxt_metrics {
    int left, right;
    int top, bottom;
};

#define HXT_IOC_MAGIC           'h'
#define HXT_IOC_SET_CS          _IOW(HXT_IOC_MAGIC, 1, __u32)
#define HXT_IOC_RESET           _IO(HXT_IOC_MAGIC, 2)
#define HXT_IOC_READY           _IO(HXT_IOC_MAGIC, 3)
#define HXT_IOC_SETUP_IRQ       _IO(HXT_IOC_MAGIC, 4)
#define HXT_IOC_WAIT_IRQ        _IOW(HXT_IOC_MAGIC, 5, __u32)
#define HXT_IOC_METRICS         _IOW(HXT_IOC_MAGIC, 6, struct hxt_metrics)

#define MAX_DATA_CHUNK          16384

/* Tracing for the bring-up: every command hx-touchd sends after the firmware
 * upload and the digitizer's answer, every ATTN with the phase it arrived in,
 * and every report read, until the budget runs out.  More on demand:
 *     echo 64 > /sys/module/apple_z2/parameters/debug */
static int hxt_dbg = 64;
module_param_named(debug, hxt_dbg, int, 0644);
MODULE_PARM_DESC(debug, "lines of boot/report tracing left to print");

#define hxt_trace(hxt, fmt, ...) do {                                   \
        if(hxt_dbg > 0) {                                               \
            hxt_dbg--;                                                  \
            dev_info(&(hxt)->spi->dev, fmt, ##__VA_ARGS__);             \
        }                                                               \
    } while(0)

struct hx_touch_data {
    struct mutex mutex;
    struct clk *clk;
    struct spi_device *spi;
    struct miscdevice misc_dev;
    struct gpio_desc *gpiod_reset;
    struct gpio_desc *gpiod_cs;
    struct gpio_desc *gpiod_irq;
    struct input_dev *input_dev;
    struct regulator *regu_hv;
    struct regulator *regu_core;
    struct regulator *regu_pmuclk;
    struct completion irq_done;
    u32 generation;
    int misc_dev_inuse;
    int ready;
    int virq;
    u8 tx_data[MAX_DATA_CHUNK];
    u8 rx_data[MAX_DATA_CHUNK];
    unsigned rx_size, rx_rdptr;
    struct hxt_metrics metrics;
    unsigned read_tag;
    unsigned nirq, nxfer, nbytes;
};

#define miscdev_to_hxt(md) container_of(md, struct hx_touch_data, misc_dev)

static void hx_touch_process_report(struct hx_touch_data *hxt, u8 *data, unsigned len)
{
    unsigned ntouch, i, finger, state;
    u8 *touch;
    long long posx, posy;
    unsigned widthm, widthu;
    s16 angle;
    int slot;

    /* The digitizer sends short reports when nothing is on the glass -- we see
     * len 12, checksum valid.  Those carry no touch block at all, so reading
     * data[16] for the count would be off the end of the packet.  Report an
     * empty frame and say nothing: this is the normal idle case, not an
     * error. */
    if(len < 24) {
        input_mt_sync_frame(hxt->input_dev);
        input_sync(hxt->input_dev);
        return;
    }

    ntouch = data[16];
    if(len < 24 + ntouch * 30) {
        dev_warn(&hxt->spi->dev, "packet too short for number of touches (%d, %d)\n", ntouch, len);
        return;
    }

    for(i=0; i<ntouch; i++) {
        touch = data + 24 + 30 * i;
        finger = touch[0];
        state = touch[1];
        posx = (s16)(touch[4] + ((unsigned)touch[5] << 8));
        posy = (s16)(touch[6] + ((unsigned)touch[7] << 8));
        widthm = touch[12] + ((unsigned)touch[13] << 8);
        widthu = touch[14] + ((unsigned)touch[15] << 8);
        angle = touch[16] + ((unsigned)touch[17] << 8);

        posx = div_s64((s64)4096 * (posx - hxt->metrics.left), hxt->metrics.right - hxt->metrics.left);
        posy = div_s64((s64)4096 * (posy - hxt->metrics.top), hxt->metrics.bottom - hxt->metrics.top);
        angle = 0x4000 - angle;

#if 0
        pr_info(">> %d,%d: %d,%d [%d:%d,%d:%d] %dx%d@%d\n", finger, state, posx, posy, hxt->metrics.left, hxt->metrics.right, hxt->metrics.top, hxt->metrics.bottom, widthm, widthu, angle);
#endif
        slot = input_mt_get_slot_by_key(hxt->input_dev, finger);
        if(slot >= 0) {
            input_mt_slot(hxt->input_dev, slot);
            input_mt_report_slot_state(hxt->input_dev, MT_TOOL_FINGER, state == 4);
            if(state == 4) {
                input_report_abs(hxt->input_dev, ABS_MT_POSITION_X, posx);
                input_report_abs(hxt->input_dev, ABS_MT_POSITION_Y, posy);
                input_report_abs(hxt->input_dev, ABS_MT_WIDTH_MAJOR, widthm);
                input_report_abs(hxt->input_dev, ABS_MT_WIDTH_MINOR, widthu);
                input_report_abs(hxt->input_dev, ABS_MT_ORIENTATION, angle);
            }
        }
    }

    input_mt_sync_frame(hxt->input_dev);
    input_sync(hxt->input_dev);
}

/* Every wait in this driver sleeps.  They were mdelay()s -- four per report
 * read, 8 ms of the only CPU spun away per ATTN -- and every caller is a
 * process: the threaded IRQ handler, open, release and the ioctls, all of
 * which already take the mutex. */
static int hx_touch_read_report(struct hx_touch_data *hxt)
{
    struct spi_transfer xfer = { 0 };
    u8 readpkt[64] = { 0xEB, 1 + hxt->read_tag };
    int ret;
    /* g1len must start at the full packet size, not 0.  When the digitizer
     * answers with zeros -- which is what a chip that never booted does --
     * readpkt[0] is 0, the branch below takes the g1done path and leaves
     * g1len alone.  With g1len = 0 the second transfer became zero-length,
     * the controller was asked for nothing, readpkt kept the command we had
     * just written into it, and this function then reported that as an
     * "invalid read header: eb 01 01 00 00" -- our own TX, mistaken for a
     * reply from the chip for several rounds of debugging. */
    unsigned len, i, g1done = 0, g1len = 16, step;
    u16 csum;

    gpiod_direction_output(hxt->gpiod_cs, 0);
    usleep_range(2000, 2500);
    readpkt[14] = 0xEC + hxt->read_tag;
    xfer.tx_buf = readpkt;
    xfer.rx_buf = readpkt;
    xfer.len = 16;
    ret = spi_sync_transfer(hxt->spi, &xfer, 1);
    gpiod_direction_output(hxt->gpiod_cs, 1);
    usleep_range(2000, 2500);
    if(ret) {
        dev_warn(&hxt->spi->dev, "spi_sync_transfer returned %d\n", ret);
        return ret;
    }
    hxt_trace(hxt, "rd hdr %16ph\n", readpkt);

    if(hxt->generation == 1) {
        if(readpkt[0] == 0) {
            g1done = 1;
        } else {
            g1len = readpkt[1] + ((unsigned)readpkt[2] << 8);
            g1len += 5;
            if(g1len < 16)
                g1len = 16;
        }
    }

    gpiod_direction_output(hxt->gpiod_cs, 0);
    usleep_range(2000, 2500);
    if(hxt->generation == 1) {
        memset(readpkt, 0, 16);
        readpkt[0] = 0xEB;
        readpkt[1] = 1 + hxt->read_tag;
        readpkt[2] = 1;
        step = 16;
        xfer.len = g1len < step ? g1len : step;
    } else {
        memset(readpkt, 0xA5, 16);
        xfer.len = step = sizeof(readpkt);
    }
    xfer.tx_buf = readpkt;
    xfer.rx_buf = readpkt;
    ret = spi_sync_transfer(hxt->spi, &xfer, 1);
    if(ret) {
        gpiod_direction_output(hxt->gpiod_cs, 1);
        dev_warn(&hxt->spi->dev, "spi_sync_transfer returned %d\n", ret);
        return ret;
    }

    hxt_trace(hxt, "rd pkt %16ph (asked %u)\n", readpkt, xfer.len);

    if(hxt->generation == 1 && (!readpkt[0] || readpkt[0] == 0xE1)) {
        gpiod_direction_output(hxt->gpiod_cs, 1);
        usleep_range(2000, 2500);
        return g1done ? -ENOENT : 0;
    }

    len = readpkt[2] + ((unsigned)readpkt[3] << 8);
    if(((readpkt[0] + readpkt[1] + readpkt[2] + readpkt[3] + readpkt[4]) & 0xFF) || len > 326 || (readpkt[0] & 0xFE) != 0xEA || readpkt[1] != 1 + hxt->read_tag) {
        gpiod_direction_output(hxt->gpiod_cs, 1);
        usleep_range(2000, 2500);
        if(readpkt[0])
            dev_warn(&hxt->spi->dev, "invalid read header: %02x %02x %02x %02x %02x\n", readpkt[0], readpkt[1], readpkt[2], readpkt[3], readpkt[4]);
        return -EINVAL;
    }

    memcpy(hxt->rx_data, readpkt + 5, step - 5);
    if(len > step - 5) {
        xfer.tx_buf = NULL;
        xfer.rx_buf = hxt->rx_data + step - 5;
        xfer.len = len;
        ret = spi_sync_transfer(hxt->spi, &xfer, 1);
        if(ret) {
            gpiod_direction_output(hxt->gpiod_cs, 1);
            dev_warn(&hxt->spi->dev, "spi_sync_transfer returned %d\n", ret);
            return ret;
        }
    }
    gpiod_direction_output(hxt->gpiod_cs, 1);

    hxt->read_tag = !hxt->read_tag;

    len -= 2;
    csum = hxt->rx_data[len] + ((unsigned)hxt->rx_data[len + 1] << 8);
    for(i=0; i<len; i++)
        csum -= hxt->rx_data[i];
    if(csum) {
        usleep_range(2000, 2500);
        dev_warn(&hxt->spi->dev, "invalid data checksum: %04x\n", csum);
        return -EINVAL;
    }

    hxt_trace(hxt, "report, %u bytes: %*ph\n", len, (int)min(len, 32u), hxt->rx_data);
    hx_touch_process_report(hxt, hxt->rx_data, len);
    usleep_range(2000, 2500);

    return 0;
}

static void hx_touch_read_all_reports(struct hx_touch_data *hxt)
{
    unsigned max;
    int ret;

    max = hxt->generation == 1 ? 4 : 1;
    while(max --) {
        ret = hx_touch_read_report(hxt);
        if(ret)
            break;
    }
}

static irqreturn_t hx_touch_irq_handler(int irq, void *dev_id)
{
    struct hx_touch_data *hxt = dev_id;

    mutex_lock(&hxt->mutex);

    hxt->nirq++;
    hxt_trace(hxt, "ATTN #%u, %s\n", hxt->nirq, hxt->ready ? "reading reports" : "boot phase");
    if(hxt->ready) {
        hx_touch_read_all_reports(hxt);
    } else
        complete(&hxt->irq_done);

    mutex_unlock(&hxt->mutex);

    return IRQ_HANDLED;
}

static int hx_touch_misc_dev_open(struct inode *inode, struct file *filp)
{
    struct hx_touch_data *hxt = miscdev_to_hxt(filp->private_data);
    int ret;

    pr_err("Z2-OPEN: enter\n");
    if(hxt->misc_dev_inuse)
        return -EBUSY;

    /* Rails first (over I2C, through the PMU), then reset and chip select,
     * then the 32 kHz clock.  An older note here said reset "clobbers SCL":
     * that was reset being put on GPIO 5, which is not the digitizer's reset
     * at all -- the ADT's 0x0205 is pin 21.  See pinctrl-s5l8940x-gpio.c. */

    /* The ADT's function-clock_enable-pmu, the one enable in the digitizer's
     * bring-up list we used to skip.  It is PMU GPIO0, and the register is not
     * in the LDO range at all: AppleD1946PMU::_setGPIOFunction in the 12H321
     * kernelcache computes it as gpio + 0x61, so GPIO0 is register 0x61.  In
     * that register 0x60 is the direction/enable field, 0x02 is the output
     * level and 0x1d is preserved configuration.  (An earlier note in
     * docs/research had this as 0x200/0x201 -- that is the LDO range, which is
     * why writing there changed nothing.)
     *
     * Without this the Z2 comes up powered and clocked but never runs its
     * bootloader: it drives nothing on MISO, so reads come back as our own TX,
     * and it never pulls ATTN, so hx-touchd times out on the boot IRQ. */
    if(hxt->regu_pmuclk) {
        pr_err("Z2-OPEN: regulator_enable pmuclk (clock_enable-pmu, PMU GPIO0 @ 0x61)\n");
        ret = regulator_enable(hxt->regu_pmuclk);
        pr_err("Z2-OPEN: regu_pmuclk ret=%d\n", ret);
        if(ret)
            return ret;
        usleep_range(5000, 6000);
    }

    pr_err("Z2-OPEN: regulator_enable hv\n");
    ret = regulator_enable(hxt->regu_hv);
    pr_err("Z2-OPEN: regu_hv ret=%d\n", ret);
    if(ret)
        return ret;
    usleep_range(5000, 6000);
    pr_err("Z2-OPEN: regulator_enable core\n");
    ret = regulator_enable(hxt->regu_core);
    pr_err("Z2-OPEN: regu_core ret=%d\n", ret);
    if(ret) {
        regulator_disable(hxt->regu_hv);
        return ret;
    }
    usleep_range(5000, 6000);

    pr_err("Z2-OPEN: reset=0\n");
    gpiod_direction_output(hxt->gpiod_reset, 0);

    pr_err("Z2-OPEN: cs=1\n");
    gpiod_direction_output(hxt->gpiod_cs, 1);

    pr_err("Z2-OPEN: clk_prepare_enable (Cmwp!)\n");
    ret = clk_prepare_enable(hxt->clk);
    pr_err("Z2-OPEN: clk ret=%d\n", ret);
    if(ret < 0)
        return ret;

    usleep_range(2000, 2500);
    pr_err("Z2-OPEN: enable_irq\n");
    enable_irq(hxt->virq);

    hxt->nirq = hxt->nxfer = hxt->nbytes = 0;
    hxt->misc_dev_inuse = 1;
    pr_err("Z2-OPEN: done\n");
    return 0;
}

static int hx_touch_misc_dev_release(struct inode *inode, struct file *filp)
{
    struct hx_touch_data *hxt = miscdev_to_hxt(filp->private_data);

    disable_irq(hxt->virq);
    gpiod_direction_output(hxt->gpiod_reset, 0);
    usleep_range(2000, 2500);
    gpiod_direction_output(hxt->gpiod_cs, 0);

    clk_disable_unprepare(hxt->clk);

    usleep_range(2000, 2500);
    regulator_disable(hxt->regu_core);
    usleep_range(2000, 2500);
    regulator_disable(hxt->regu_hv);
    if(hxt->regu_pmuclk) {
        usleep_range(2000, 2500);
        regulator_disable(hxt->regu_pmuclk);
    }

    hxt->rx_size = hxt->rx_rdptr = 0;
    hxt->misc_dev_inuse = 0;
    hxt->ready = 0;
    return 0;
}

static ssize_t hx_touch_misc_dev_write(struct file *filp, const char __user *udata, size_t sz, loff_t *offp)
{
    struct hx_touch_data *hxt = miscdev_to_hxt(filp->private_data);
    struct spi_transfer xfer = { 0 };
    unsigned long retsz;
    int ret;

    mutex_lock(&hxt->mutex);
    if(hxt->ready) {
        mutex_unlock(&hxt->mutex);
        return -EINVAL;
    }

    if(sz > MAX_DATA_CHUNK || sz + hxt->rx_size > MAX_DATA_CHUNK) {
        mutex_unlock(&hxt->mutex);
        return -ENOSPC;
    }

    retsz = copy_from_user(hxt->tx_data, udata, sz);
    if(retsz) {
        mutex_unlock(&hxt->mutex);
        return -EFAULT;
    }

    xfer.tx_buf = hxt->tx_data;
    xfer.rx_buf = hxt->rx_data + hxt->rx_size;
    xfer.len = sz;
    ret = spi_sync_transfer(hxt->spi, &xfer, 1);
    if(ret < 0) {
        dev_warn(&hxt->spi->dev, "spi_sync_transfer returned %d.\n", ret);
        mutex_unlock(&hxt->mutex);
        return ret;
    }

    /* The commands hx-touchd sends once the firmware is in (0xE1..0xEE, 16
     * bytes each) and what comes back; the upload itself is only counted. */
    if(sz == 16 && hxt->tx_data[0] >= 0xE0)
        hxt_trace(hxt, "cmd %16ph -> %16ph\n", hxt->tx_data, hxt->rx_data + hxt->rx_size);
    else {
        hxt->nxfer++;
        hxt->nbytes += sz;
    }

    hxt->rx_size += sz;
    mutex_unlock(&hxt->mutex);
    return sz;
}

static ssize_t hx_touch_misc_dev_read(struct file *filp, char __user *udata, size_t sz, loff_t *offp)
{
    struct hx_touch_data *hxt = miscdev_to_hxt(filp->private_data);
    unsigned long ret;

    mutex_lock(&hxt->mutex);
    if(hxt->ready) {
        mutex_unlock(&hxt->mutex);
        return -EINVAL;
    }

    if(sz > hxt->rx_size - hxt->rx_rdptr)
        sz = hxt->rx_size - hxt->rx_rdptr;
    if(!sz) {
        mutex_unlock(&hxt->mutex);
        return 0;
    }

    ret = copy_to_user(udata, hxt->rx_data + hxt->rx_rdptr, sz);
    if(ret) {
        mutex_unlock(&hxt->mutex);
        return -EFAULT;
    }

    hxt->rx_rdptr += sz;
    if(hxt->rx_rdptr >= hxt->rx_size)
        hxt->rx_size = hxt->rx_rdptr = 0;
    mutex_unlock(&hxt->mutex);
    return sz;
}

static long hx_touch_misc_dev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct hx_touch_data *hxt = miscdev_to_hxt(filp->private_data);
    unsigned long timeout;

    switch(cmd) {
    case HXT_IOC_SET_CS:
        if(hxt->ready)
            return -EINVAL;
        gpiod_direction_output(hxt->gpiod_cs, !arg);
        return 0;
    case HXT_IOC_RESET:
        mutex_lock(&hxt->mutex);
        hxt->ready = 0;
        gpiod_direction_output(hxt->gpiod_cs, 1);
        usleep_range(1000, 1500);
        gpiod_direction_output(hxt->gpiod_reset, 0);
        usleep_range(2000, 2500);
        gpiod_direction_output(hxt->gpiod_reset, 1);
        mutex_unlock(&hxt->mutex);
        return 0;
    case HXT_IOC_READY:
        if(hxt->ready)
            return -EINVAL;
        gpiod_direction_output(hxt->gpiod_cs, 1);

        mutex_lock(&hxt->mutex);
        hxt_trace(hxt, "READY after %u ATTN, %u other transfers (%u bytes); metrics x %d..%d y %d..%d\n",
                  hxt->nirq, hxt->nxfer, hxt->nbytes, hxt->metrics.left, hxt->metrics.right,
                  hxt->metrics.top, hxt->metrics.bottom);
        hxt->ready = 1;
        hx_touch_read_all_reports(hxt);
        mutex_unlock(&hxt->mutex);
        return 0;
    case HXT_IOC_SETUP_IRQ:
        if(hxt->ready)
            return -EINVAL;
        reinit_completion(&hxt->irq_done);
        return 0;
    case HXT_IOC_WAIT_IRQ:
        if(hxt->ready)
            return -EINVAL;
        timeout = msecs_to_jiffies(arg);
        timeout = wait_for_completion_timeout(&hxt->irq_done, timeout);
        return timeout ? 0 : -EAGAIN;
    case HXT_IOC_METRICS:
        mutex_lock(&hxt->mutex);
        if(hxt->ready) {
            mutex_unlock(&hxt->mutex);
            return -EINVAL;
        }
        if(copy_from_user(&hxt->metrics, (void __user *)arg, sizeof(hxt->metrics))) {
            mutex_unlock(&hxt->mutex);
            return -EFAULT;
        }
        if(hxt->metrics.left == hxt->metrics.right) {
            hxt->metrics.left = 0;
            hxt->metrics.right = 10000;
        }
        if(hxt->metrics.top == hxt->metrics.bottom) {
            hxt->metrics.top = 0;
            hxt->metrics.bottom = 10000;
        }
        mutex_unlock(&hxt->mutex);
        return 0;
    }
    return -ENOTTY;
}

static const struct file_operations hx_touch_misc_dev_fops = {
    .owner = THIS_MODULE,
    .open = hx_touch_misc_dev_open,
    .release = hx_touch_misc_dev_release,
    .read = hx_touch_misc_dev_read,
    .write = hx_touch_misc_dev_write,
    .unlocked_ioctl = hx_touch_misc_dev_ioctl,
};

static int hx_touch_spi_create_touch_input(struct hx_touch_data *hxt)
{
    struct input_dev *input;
    int ret;

    input = devm_input_allocate_device(&hxt->spi->dev);
    if(!input)
        return -ENOMEM;

    hxt->input_dev = input;

    input_set_abs_params(input, ABS_MT_POSITION_X, 0, 4096, 0, 0);
    input_abs_set_res(input, ABS_MT_POSITION_X, 1);
    input_set_abs_params(input, ABS_MT_POSITION_Y, 0, 4096, 0, 0);
    input_abs_set_res(input, ABS_MT_POSITION_Y, 1);
    input_set_abs_params(input, ABS_MT_WIDTH_MAJOR, 0, 65535, 0, 0);
    input_set_abs_params(input, ABS_MT_WIDTH_MINOR, 0, 65535, 0, 0);
    input_set_abs_params(input, ABS_MT_ORIENTATION, -32768, 32767, 0, 0);
    input_mt_init_slots(input, 10, INPUT_MT_DIRECT);

    input->name = "S5L8940X Capacitive TouchScreen";
    input->phys = "input/ts";
    input->id.bustype = BUS_SPI;
    input->id.vendor = 0x05ac;
    input->id.product = 0x0001;
    input->id.version = 0x0000;

    ret = input_register_device(input);
    if(ret) {
        dev_err(&hxt->spi->dev, "failed to register input device: %d.", ret);
        return ret;
    }

    return 0;
}

static int hx_touch_spi_probe(struct spi_device *spi)
{
    struct hx_touch_data *hxt;
    struct miscdevice *misc_dev;
    int ret;

    spi->bits_per_word = 8;
    spi->mode = SPI_MODE_0;
    ret = spi_setup(spi);
    if(ret)
        return ret;

    hxt = devm_kzalloc(&spi->dev, sizeof(*hxt), GFP_KERNEL);
    if(!hxt)
        return -ENOMEM;

    mutex_init(&hxt->mutex);
    init_completion(&hxt->irq_done);

    hxt->spi = spi;
    spi_set_drvdata(spi, hxt);

    hxt->regu_hv = devm_regulator_get(&spi->dev, "hv");
    if(IS_ERR(hxt->regu_hv)) {
        if(PTR_ERR(hxt->regu_hv) == -EPROBE_DEFER)
            return PTR_ERR(hxt->regu_hv);
        dev_warn(&spi->dev, "touch panel may not function, missing 'hv-supply': %ld.", PTR_ERR(hxt->regu_hv));
        hxt->regu_hv = NULL;
    }

    hxt->regu_core = devm_regulator_get(&spi->dev, "core");
    if(IS_ERR(hxt->regu_core)) {
        if(PTR_ERR(hxt->regu_core) == -EPROBE_DEFER)
            return PTR_ERR(hxt->regu_core);
        dev_warn(&spi->dev, "touch panel may not function, missing 'core-supply': %ld.", PTR_ERR(hxt->regu_core));
        hxt->regu_core = NULL;
    }

    hxt->regu_pmuclk = devm_regulator_get(&spi->dev, "pmuclk");
    if(IS_ERR(hxt->regu_pmuclk)) {
        if(PTR_ERR(hxt->regu_pmuclk) == -EPROBE_DEFER)
            return PTR_ERR(hxt->regu_pmuclk);
        dev_warn(&spi->dev, "no 'pmuclk-supply' (ADT clock_enable-pmu): %ld.", PTR_ERR(hxt->regu_pmuclk));
        hxt->regu_pmuclk = NULL;
    }

    hxt->gpiod_reset = devm_gpiod_get_index(&spi->dev, "reset", 0, 0);
    if(IS_ERR(hxt->gpiod_reset)) {
        if(PTR_ERR(hxt->gpiod_reset) != -EPROBE_DEFER)
            dev_err(&spi->dev, "failed to get 'reset-gpios': %ld\n", PTR_ERR(hxt->gpiod_reset));
        return PTR_ERR(hxt->gpiod_reset);
    }

    hxt->gpiod_cs = devm_gpiod_get_index(&spi->dev, "cs", 0, 0);
    if(IS_ERR(hxt->gpiod_cs)) {
        if(PTR_ERR(hxt->gpiod_cs) != -EPROBE_DEFER)
            dev_err(&spi->dev, "failed to get 'cs-gpios': %ld\n", PTR_ERR(hxt->gpiod_cs));
        return PTR_ERR(hxt->gpiod_cs);
    }

    hxt->gpiod_irq = devm_gpiod_get_index(&spi->dev, "irq", 0, 0);
    if(IS_ERR(hxt->gpiod_irq)) {
        if(PTR_ERR(hxt->gpiod_irq) != -EPROBE_DEFER)
            dev_err(&spi->dev, "failed to get 'irq-gpios': %ld\n", PTR_ERR(hxt->gpiod_irq));
        return PTR_ERR(hxt->gpiod_irq);
    }

    hxt->clk = devm_clk_get(&spi->dev, NULL);
    if(IS_ERR(hxt->clk))
        return PTR_ERR(hxt->clk);

    hxt->virq = gpiod_to_irq(hxt->gpiod_irq);
    if(hxt->virq < 0) {
        dev_err(&spi->dev, "IRQ GPIO is not usable as an IRQ: %d.\n", hxt->virq);
        return hxt->virq;
    }

    if(of_property_read_u32_index(spi->dev.of_node, "generation", 0, &hxt->generation))
        hxt->generation = 2;

    hxt->metrics.left = 0;
    hxt->metrics.right = 10000;
    hxt->metrics.top = 0;
    hxt->metrics.bottom = 10000;

    gpiod_direction_output(hxt->gpiod_reset, 0);
    gpiod_direction_output(hxt->gpiod_cs, 0);

    ret = hx_touch_spi_create_touch_input(hxt);
    if(ret)
        return ret;

    ret = devm_request_threaded_irq(&spi->dev, hxt->virq, NULL, hx_touch_irq_handler, IRQF_TRIGGER_FALLING | IRQF_ONESHOT, "apple-z2", hxt);
    if(ret)
        return ret;
    disable_irq(hxt->virq);

    misc_dev = &hxt->misc_dev;
    misc_dev->minor = MISC_DYNAMIC_MINOR;
    misc_dev->name = "hx-touch";
    misc_dev->fops = &hx_touch_misc_dev_fops;
    ret = misc_register(misc_dev);
    if(ret) {
        dev_err(&spi->dev, "failed to register control device: %d.\n", ret);
        return ret;
    }

    return 0;
}

static const struct of_device_id hx_touch_of_match[] = {
    { .compatible = "apple,z2-multitouch" },
    { },
};

static struct spi_driver hx_touch_spi_driver = {
    .driver = {
        .name = "apple-z2",
        .of_match_table = of_match_ptr(hx_touch_of_match),
    },
    .probe = hx_touch_spi_probe,
};

module_spi_driver(hx_touch_spi_driver);

MODULE_DESCRIPTION("S5L8940X multi-touch driver");
MODULE_AUTHOR("Corellium LLC");
MODULE_LICENSE("GPL v2");
