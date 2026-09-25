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
#include <linux/input/touchscreen.h>
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

/* Tracing for the bring-up: every command sent after the firmware upload and
 * the digitizer's answer, every ATTN with the phase it arrived in, every frame
 * read and every touch in it, until the budget runs out.  Off now that it all
 * works; on demand:
 *     echo 300 > /sys/module/apple_z2/parameters/debug */
static int hxt_dbg;
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
    struct touchscreen_properties prop;
    unsigned read_tag;
    unsigned nirq, nxfer, nbytes, nframes, nretries;
};

#define miscdev_to_hxt(md) container_of(md, struct hx_touch_data, misc_dev)

/* A frame's payload, as P105's digitizer sends it and iOS 8.4.1 reads it
 * (logs/ios-mtlog1.txt; docs/research/p105-z2-boot.md): [16] is the number
 * of touches, and from [24] each touch is 30 bytes -- [0] id, [1] state,
 * [4..5] x, [6..7] y (signed, the units of report d9's surface), [12..13]
 * and [14..15] the major and minor width, [16..17] the angle.  States are
 * Apple's path stages: 3 make-touch and 4 touching mean a finger on the
 * glass, anything else is on its way off.  There are no frames without a
 * finger: the last one of a touch carries its lift. */
static void hx_touch_process_report(struct hx_touch_data *hxt, u8 *data, unsigned len)
{
    unsigned ntouch, i, finger, state, down;
    u8 *touch;
    long long posx, posy;
    unsigned widthm, widthu;
    s16 angle;
    int slot;

    if(len < 24) {
        input_mt_sync_frame(hxt->input_dev);
        input_sync(hxt->input_dev);
        return;
    }

    ntouch = data[16];
    if(len < 24 + ntouch * 30) {
        dev_warn_ratelimited(&hxt->spi->dev, "frame too short for %u touches (%u bytes)\n", ntouch, len);
        return;
    }

    for(i=0; i<ntouch; i++) {
        touch = data + 24 + 30 * i;
        finger = touch[0];
        state = touch[1];
        down = state == 3 || state == 4;
        posx = (s16)(touch[4] | ((unsigned)touch[5] << 8));
        posy = (s16)(touch[6] | ((unsigned)touch[7] << 8));
        widthm = touch[12] | ((unsigned)touch[13] << 8);
        widthu = touch[14] | ((unsigned)touch[15] << 8);
        angle = touch[16] | ((unsigned)touch[17] << 8);

        hxt_trace(hxt, "touch id %u state %u x %lld y %lld w %u/%u\n",
                  finger, state, posx, posy, widthm, widthu);

        posx = div_s64((s64)4096 * (posx - hxt->metrics.left), hxt->metrics.right - hxt->metrics.left);
        posy = div_s64((s64)4096 * (posy - hxt->metrics.top), hxt->metrics.bottom - hxt->metrics.top);
        posx = clamp_val(posx, 0, 4096);
        posy = clamp_val(posy, 0, 4096);
        angle = 0x4000 - angle;

        slot = input_mt_get_slot_by_key(hxt->input_dev, finger);
        if(slot < 0)
            continue;
        input_mt_slot(hxt->input_dev, slot);
        input_mt_report_slot_state(hxt->input_dev, MT_TOOL_FINGER, down);
        if(down) {
            /* the panel's y grows bottom to top -- touchscreen-inverted-y */
            touchscreen_report_pos(hxt->input_dev, &hxt->prop, posx, posy, true);
            input_report_abs(hxt->input_dev, ABS_MT_WIDTH_MAJOR, widthm);
            input_report_abs(hxt->input_dev, ABS_MT_WIDTH_MINOR, widthu);
            input_report_abs(hxt->input_dev, ABS_MT_ORIENTATION, angle);
        }
    }

    input_mt_sync_frame(hxt->input_dev);
    input_sync(hxt->input_dev);
}

static u16 hx_touch_sum16(const u8 *p, unsigned n)
{
    u16 s = 0;

    while(n--)
        s += *p++;
    return s;
}

/* One chip-select window, n bytes each way -- z2-boot's xfer(), whose
 * 1 ms either side of the transfer the chip has been happy with. */
static int hx_touch_xfer(struct hx_touch_data *hxt, const u8 *tx, u8 *rx, unsigned n)
{
    struct spi_transfer xfer = { .tx_buf = tx, .rx_buf = rx, .len = n };
    int ret;

    gpiod_direction_output(hxt->gpiod_cs, 0);
    usleep_range(1000, 1200);
    ret = spi_sync_transfer(hxt->spi, &xfer, 1);
    gpiod_direction_output(hxt->gpiod_cs, 1);
    usleep_range(1000, 1200);
    return ret;
}

#define Z2_MAX_PACKET   1944    /* device info e2: 0x798 */

/* A frame, the way AppleMultitouchZ2SPI reads one (12H321: result length
 * 0x80605be0, result data 0x80605d5c), which the old read -- a 16-byte
 * header, then the rest with no command and no checksum, frames capped at
 * 326 bytes -- was not:
 *   eb seq 00.. csum16([0..13]) at [14..15]    -> eX LL.. csum at [14..15]
 *   eb seq 01 00.. csum16([0..13]) at the end, LL + 5 bytes
 *                                              -> ea seq LL hcsum payload csum
 * the first five answer bytes sum to 0, the payload's sum16 is in the last
 * two; seq goes 1, 2, 1, ... after each good read.  -ENOENT: nothing to read. */
static int hx_touch_read_report(struct hx_touch_data *hxt)
{
    u8 *q = hxt->tx_data, *r = hxt->rx_data;
    unsigned seq = 1 + hxt->read_tag, len, n, plen, tries;
    u16 s;
    int ret;

    /* A query answered with a length, then the data answered with the
     * status again (e1 38 00 eb 01 ..: the chip still on the query) is a
     * frame not ready yet -- ask for the length again and read again, as
     * AppleMultitouchZ2SPI does after a failed read (its +0x823). */
    for(tries = 0; tries < 4; tries++) {
        memset(q, 0, 16);
        q[0] = 0xeb;
        q[1] = seq;
        s = hx_touch_sum16(q, 14);
        q[14] = s;
        q[15] = s >> 8;
        ret = hx_touch_xfer(hxt, q, r, 16);
        if(ret)
            return ret;
        hxt_trace(hxt, "q %u: %16ph\n", seq, r);
        if((r[0] & 0xf0) != 0xe0 || (r[14] | r[15] << 8) != hx_touch_sum16(r, 14))
            continue;
        len = r[1] | r[2] << 8;
        if(!len)
            return -ENOENT;
        if(len > Z2_MAX_PACKET) {
            dev_warn_ratelimited(&hxt->spi->dev, "result length %u, more than %u\n", len, Z2_MAX_PACKET);
            return -EIO;
        }

        n = len + 5;
        memset(q, 0, n);
        q[0] = 0xeb;
        q[1] = seq;
        q[2] = 1;
        s = hx_touch_sum16(q, 14);
        q[n - 2] = s;
        q[n - 1] = s >> 8;
        ret = hx_touch_xfer(hxt, q, r, n);
        if(ret)
            return ret;
        hxt_trace(hxt, "d %u: %16ph (%u)\n", seq, r, n);
        if(r[0] == 0xea || r[0] == 0xeb)
            break;
        hxt->nretries++;
    }
    if(tries == 4) {
        dev_warn_ratelimited(&hxt->spi->dev, "no frame after 4 tries: %5ph\n", r);
        return -EIO;
    }

    plen = r[2] | r[3] << 8;
    if(r[1] != seq || ((r[0] + r[1] + r[2] + r[3] + r[4]) & 0xff) || plen < 2 || plen + 5 > n) {
        dev_warn_ratelimited(&hxt->spi->dev, "bad frame header %5ph (asked %u)\n", r, n);
        return -EIO;
    }
    if(hx_touch_sum16(r + 5, plen - 2) != (r[plen + 3] | r[plen + 4] << 8)) {
        dev_warn_ratelimited(&hxt->spi->dev, "bad frame checksum (%u bytes)\n", plen);
        return -EIO;
    }
    hxt->read_tag = !hxt->read_tag;
    hxt->nframes++;
    hx_touch_process_report(hxt, r + 5, plen - 2);
    return 0;
}

/* Read while ATTN stays asserted: the interrupt is on its falling edge, and
 * a frame left unread leaves the line low with no edge to come -- which is
 * also why a failed read does not end the loop while the line is down. */
static void hx_touch_read_all_reports(struct hx_touch_data *hxt)
{
    int i, ret;

    for(i = 0; i < 16; i++) {
        ret = hx_touch_read_report(hxt);
        if(gpiod_get_value(hxt->gpiod_irq) <= 0)
            break;
        if(ret)
            usleep_range(1000, 2000);
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

    hxt->nirq = hxt->nxfer = hxt->nbytes = hxt->nframes = hxt->nretries = 0;
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
    touchscreen_parse_properties(input, true, &hxt->prop);
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
