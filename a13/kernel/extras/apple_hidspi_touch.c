// SPDX-License-Identifier: GPL-2.0
/*
 * Touchscreen of the iPhone 11 (N104, A13): an Apple multitouch controller
 * on SPI with no flash of its own. The host loads its firmware through the
 * HBPP bootloader on every power-up; the firmware then speaks Apple's
 * HID-over-SPI ("HIDSPI") transport, and touches arrive as input report 0x75.
 * See a13/docs/research/touch.md in Project Cascadia for how all of this
 * was found.
 *
 * The finger records and their parsing are those of Linux's apple_z2.c
 * (Sasha Finkelstein, Asahi Linux); the firmware file is the "Z2FW" format
 * that apple_z2 reads (asahi-installer's converter, here a13/tools/mtfw.py).
 *
 * Bootloader (HBPP, as on the A5's controller, not as on the Touch Bar):
 * every packet goes out as 16-bit words (byte pairs swapped), data packets
 * behind an `18 e1`, and each one is answered, 20 ms later, by an ATN_ACK
 * (`1a a1`) of its own: 4bc1 data taken, 4ad1 register written.
 *
 * HIDSPI (from iOS's AppleHIDTransportSPI, checked on the phone):
 *  - a transfer is [4 bytes padding][512-byte packet] both ways: type,
 *    device, offset, remain, length, data, crc16 over the first 510 bytes;
 *  - in the same chip-select window, 200 us later, a status stage:
 *    [padding][u32] both ways, 0xd56827ac = OK;
 *  - when the host has nothing to send it clocks out 0xff: an all-zero
 *    packet has a valid CRC and makes the firmware fall back to its
 *    bootloader (which this driver uses as its reset);
 *  - with an interface config (the firmware plist has one) there is no
 *    enumeration; touches simply arrive on interface 0.
 *
 * The interrupt line (AOP GPIO 34) does not move here, so the controller is
 * polled: often while a finger is down, rarely otherwise.
 */

#include <linux/crc16.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/kthread.h>
#include <linux/mfd/macsmc.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>

#define FW_MAGIC		0x5746325a	/* "Z2FW" */
#define LOAD_INIT		0
#define LOAD_BLOB		1
#define LOAD_CAL		2

#define HBPP_CHUNK		0x3f0
#define HBPP_DATA_OK		0x4bc1
#define HBPP_REG_OK		0x4ad1

#define HS_PAD			4
#define HS_LEN			512
#define HS_XFER			(HS_PAD + HS_LEN)
#define HS_ST_OK		0xd56827ac
#define HS_ST_NAK		0x0e4fb139
#define BUF_LEN			1100	/* a bootloader packet: 18 e1 + 14 + 0x3f0 */
#define HS_TYPE_DEV_INPUT	0x20	/* device -> host, interface reports */
#define HS_IFACE		0x00	/* "grape" */
#define HS_REPORT_TOUCH		0x75

#define MAX_FINGERS		16
#define FINGERS_OFFSET		32	/* in report 0x75 */
#define FINGER_LEN		30
#define STATE_DOWN		3
#define STATE_MOVED		4

struct finger {
	u8 id;
	u8 state;
	__le16 unknown2;
	__le16 abs_x;
	__le16 abs_y;
	__le16 rel_x;
	__le16 rel_y;
	__le16 tool_major;
	__le16 tool_minor;
	__le16 orientation;
	__le16 touch_major;
	__le16 touch_minor;
	__le16 unused[2];
	__le16 pressure;
	__le16 multi;
} __packed;

struct ahtouch {
	struct spi_device *spi;
	struct input_dev *input;
	struct touchscreen_properties props;
	struct task_struct *thread;
	const char *fw_name;
	const char *cal_name;
	u8 *tx, *rx;		/* DMA-safe, BUF_LEN */
	u8 *last;		/* the previous HIDSPI transfer */
	u8 *pkt;		/* a bootloader packet being built */
	unsigned int fails;
	/* power, as the ADT's function-power_ana / function-clock_enable */
	struct i2c_client *pmu;
	u32 pmu_reg, pmu_bit;
	struct apple_smc *smc;
	u32 smc_key, smc_val;
};

static unsigned int poll_active_ms = 8;
module_param(poll_active_ms, uint, 0644);
MODULE_PARM_DESC(poll_active_ms, "poll period while a finger is down");
static unsigned int poll_idle_ms = 25;
module_param(poll_idle_ms, uint, 0644);
MODULE_PARM_DESC(poll_idle_ms, "poll period otherwise");

/* ---- SPI ---------------------------------------------------------------- */

static int xfer(struct ahtouch *ts, size_t len)
{
	struct spi_transfer t = {
		.tx_buf = ts->tx,
		.rx_buf = ts->rx,
		.len = len,
	};

	return spi_sync_transfer(ts->spi, &t, 1);
}

/* ---- HBPP bootloader ---------------------------------------------------- */

static int hbpp_ack(struct ahtouch *ts)
{
	int ret;

	ts->tx[0] = 0x1a;
	ts->tx[1] = 0xa1;
	ret = xfer(ts, 2);
	return ret ? ret : get_unaligned_be16(ts->rx);
}

static bool hbpp_status_word(u16 w)
{
	return w == 0x18e1 || w == 0x1aa1 || w == 0x1f01 || w == 0x4879 ||
	       w == 0x4969 || w == 0x4ad1 || w == 0x4bc1;
}

static bool hbpp_check(struct ahtouch *ts)
{
	int i;

	ts->tx[0] = 0x1a;
	ts->tx[1] = 0xa1;
	for (i = 2; i < 16; i += 2) {
		ts->tx[i] = 0x18;
		ts->tx[i + 1] = 0xe1;
	}
	if (xfer(ts, 16))
		return false;
	return hbpp_status_word(get_unaligned_be16(ts->rx)) &&
	       hbpp_status_word(get_unaligned_be16(ts->rx + 2));
}

/* one packet of the file (LE halfwords), sent as 16-bit words */
static int hbpp_send(struct ahtouch *ts, const u8 *pkt, size_t len)
{
	u16 cmd = get_unaligned_le16(pkt);
	bool data = cmd == 0x3001;
	int want = cmd == 0x1e33 ? HBPP_REG_OK : HBPP_DATA_OK;
	size_t i, off = data ? 2 : 0;
	int st = -EIO, tries;

	if (len & 1 || len + off > BUF_LEN)
		return -EINVAL;
	for (tries = 0; tries < 5; tries++) {
		ts->tx[0] = 0x18;
		ts->tx[1] = 0xe1;
		for (i = 0; i < len; i++)
			ts->tx[off + i] = pkt[i ^ 1];
		st = xfer(ts, len + off);
		if (st)
			return st;
		msleep(20);
		st = hbpp_ack(ts);
		if (cmd == 0x1f01 || st == want)	/* calibration request: no fixed answer */
			return 0;
	}
	dev_err(&ts->spi->dev, "HBPP packet %04x: status %04x\n", cmd, st);
	return -EIO;
}

/* a 0x3001 blob (header, csum16, payload, sum32), resent in pieces */
static int hbpp_blob(struct ahtouch *ts, const u8 *b, size_t len)
{
	u8 *pkt = ts->pkt;
	u32 addr = get_unaligned_le32(b + 4), sum;
	size_t plen = len - 14, off, n, i;
	int ret;

	for (off = 0; off < plen; off += n) {
		n = min_t(size_t, plen - off, HBPP_CHUNK);
		put_unaligned_le16(0x3001, pkt);
		put_unaligned_le16((n + 3) / 4, pkt + 2);
		put_unaligned_le32(addr + off, pkt + 4);
		put_unaligned_le16(pkt[2] + pkt[3] + pkt[4] + pkt[5] + pkt[6] + pkt[7], pkt + 8);
		memcpy(pkt + 10, b + 10 + off, n);
		for (sum = 0, i = 0; i < n; i++)
			sum += b[10 + off + i];
		put_unaligned_le32(sum, pkt + 10 + n);
		ret = hbpp_send(ts, pkt, 14 + n);
		if (ret)
			return ret;
	}
	return 0;
}

static int hbpp_calibration(struct ahtouch *ts, u32 addr)
{
	struct device *dev = &ts->spi->dev;
	const struct firmware *fw = NULL;
	const u8 *cal;
	u8 *b;
	int len, ret, i;
	u32 sum = 0;

	/* the panel's own (iBoot's MtCl, copied by m1n1), else the plist's default */
	len = device_property_count_u8(dev, "apple,z2-cal-blob");
	if (len > 16) {		/* a real blob, not the ADT's "syscfg/MtCl" placeholder */
		cal = NULL;
	} else {
		if (!ts->cal_name || firmware_request_nowarn(&fw, ts->cal_name, dev)) {
			dev_info(dev, "no calibration, skipped\n");
			return 0;
		}
		cal = fw->data;
		len = fw->size;
	}
	b = kzalloc(14 + len, GFP_KERNEL);
	if (!b) {
		release_firmware(fw);
		return -ENOMEM;
	}
	put_unaligned_le16(0x3001, b);
	put_unaligned_le16((len + 3) / 4, b + 2);
	put_unaligned_le32(addr, b + 4);
	put_unaligned_le16(b[2] + b[3] + b[4] + b[5] + b[6] + b[7], b + 8);
	if (cal)
		memcpy(b + 10, cal, len);
	else
		device_property_read_u8_array(dev, "apple,z2-cal-blob", b + 10, len);
	for (i = 0; i < len; i++)
		sum += b[10 + i];
	put_unaligned_le32(sum, b + 10 + len);
	dev_info(dev, "calibration: %d bytes (%s)\n", len, cal ? ts->cal_name : "panel");
	ret = hbpp_blob(ts, b, 14 + len);
	kfree(b);
	release_firmware(fw);
	return ret;
}

static int hbpp_load(struct ahtouch *ts)
{
	struct device *dev = &ts->spi->dev;
	const struct firmware *fw;
	size_t off = 8;
	int ret;

	ret = request_firmware(&fw, ts->fw_name, dev);
	if (ret)
		return ret;
	if (fw->size < 8 || get_unaligned_le32(fw->data) != FW_MAGIC ||
	    get_unaligned_le32(fw->data + 4) != 1) {
		ret = -EINVAL;
		goto out;
	}
	while (off + 8 <= fw->size) {
		u32 cmd = get_unaligned_le32(fw->data + off);
		u32 arg = get_unaligned_le32(fw->data + off + 4);
		const u8 *p = fw->data + off + 8;

		off += 8;
		if (cmd == LOAD_INIT || cmd == LOAD_BLOB) {
			if (arg > fw->size - off) {
				ret = -EINVAL;
				goto out;
			}
			if (cmd == LOAD_BLOB && arg >= 14 && get_unaligned_le16(p) == 0x3001)
				ret = hbpp_blob(ts, p, arg);
			else if (cmd == LOAD_BLOB)
				ret = hbpp_send(ts, p, arg);
			else
				ret = 0;	/* no init payload on this controller */
			off += arg;
		} else if (cmd == LOAD_CAL) {
			ret = hbpp_calibration(ts, arg);
		} else {
			ret = -EINVAL;
		}
		if (ret)
			goto out;
		off = ALIGN(off, 4);
	}
out:
	release_firmware(fw);
	if (ret)
		dev_err(dev, "firmware %s: %d\n", ts->fw_name, ret);
	return ret;
}

/* ---- HIDSPI ------------------------------------------------------------- */

/*
 * One exchange: our packet (or 0xff) for the device's, then the status stage
 * in the same chip-select window. Returns true if the device sent a packet
 * whose CRC (of the packet, or of its message) holds.
 */
static bool hs_exchange(struct ahtouch *ts)
{
	struct spi_transfer t[2] = {
		{ .delay = { .value = 300, .unit = SPI_DELAY_UNIT_USECS } },	/* CS setup */
		{ .tx_buf = ts->tx, .rx_buf = ts->rx, .len = HS_XFER, .cs_change = 1 },
	};
	struct spi_transfer st = { .tx_buf = ts->tx, .rx_buf = ts->rx + HS_XFER, .len = HS_PAD + 4 };
	const u8 *pk = ts->rx + HS_PAD;
	u16 len;
	bool ok;

	memset(ts->tx, 0xff, HS_XFER);
	if (spi_sync_transfer(ts->spi, t, 2))
		return false;

	len = get_unaligned_le16(pk + 6);
	ok = crc16(0, pk, HS_LEN - 2) == get_unaligned_le16(pk + HS_LEN - 2);
	if (!ok && len >= 10 && len <= HS_LEN - 10) {
		u16 mlen = get_unaligned_le16(pk + 8 + 6);

		ok = mlen + 10 <= len &&
		     crc16(0, pk + 8, 8 + mlen) == get_unaligned_le16(pk + 16 + mlen);
	}
	if (!ok && len == 4 && pk[0] == HS_TYPE_DEV_INPUT)	/* messages carry no CRC */
		ok = true;

	usleep_range(200, 250);			/* Inter Stage Us */
	memset(ts->tx, 0xff, HS_PAD);
	put_unaligned_le32(ok ? HS_ST_OK : HS_ST_NAK, ts->tx + HS_PAD);
	spi_sync_transfer(ts->spi, &st, 1);
	usleep_range(250, 300);			/* Inter Transaction Us */
	return ok && get_unaligned_le32(ts->rx) == 0xdeadbeef;
}

static void report_touches(struct ahtouch *ts, const u8 *p, size_t len)
{
	struct input_dev *input = ts->input;
	u32 bytes;
	int i, n;

	if (len < FINGERS_OFFSET)
		return;
	bytes = get_unaligned_le32(p + 16);
	n = min_t(int, bytes / FINGER_LEN, (len - FINGERS_OFFSET) / FINGER_LEN);
	for (i = 0; i < n; i++) {
		const struct finger *f = (const void *)(p + FINGERS_OFFSET + i * FINGER_LEN);
		int slot = input_mt_get_slot_by_key(input, f->id);
		bool down = f->state == STATE_DOWN || f->state == STATE_MOVED;

		if (slot < 0)
			continue;
		input_mt_slot(input, slot);
		if (!input_mt_report_slot_state(input, MT_TOOL_FINGER, down))
			continue;
		touchscreen_report_pos(input, &ts->props,
				       max_t(int, (s16)le16_to_cpu(f->abs_x), 0),
				       max_t(int, (s16)le16_to_cpu(f->abs_y), 0), true);
		input_report_abs(input, ABS_MT_WIDTH_MAJOR, le16_to_cpu(f->tool_major));
		input_report_abs(input, ABS_MT_WIDTH_MINOR, le16_to_cpu(f->tool_minor));
		input_report_abs(input, ABS_MT_ORIENTATION, (s16)le16_to_cpu(f->orientation));
		input_report_abs(input, ABS_MT_TOUCH_MAJOR, le16_to_cpu(f->touch_major));
		input_report_abs(input, ABS_MT_TOUCH_MINOR, le16_to_cpu(f->touch_minor));
	}
	input_mt_sync_frame(input);
	input_sync(input);
}

/* returns true if a touch frame was reported */
static bool hs_poll(struct ahtouch *ts)
{
	const u8 *pk = ts->rx + HS_PAD, *m = pk + 8;
	u16 len, mlen;

	if (!hs_exchange(ts)) {
		ts->fails++;
		return false;
	}
	ts->fails = 0;
	if (!memcmp(ts->last, ts->rx, HS_XFER))
		return false;			/* the same packet again: nothing new */
	memcpy(ts->last, ts->rx, HS_XFER);

	len = get_unaligned_le16(pk + 6);
	if (pk[0] != HS_TYPE_DEV_INPUT || pk[1] != HS_IFACE || len < 10)
		return false;
	mlen = get_unaligned_le16(m + 6);
	if (m[0] != 0x10 || m[1] != HS_REPORT_TOUCH || mlen + 10 > len)
		return false;
	report_touches(ts, m + 8, mlen);
	return true;
}

/* ---- power, boot, thread ------------------------------------------------ */

static int power_on(struct ahtouch *ts)
{
	struct device *dev = &ts->spi->dev;
	int ret;

	if (ts->pmu) {
		ret = i2c_smbus_read_byte_data(ts->pmu, ts->pmu_reg);
		if (ret < 0)
			return ret;
		if (!(ret & ts->pmu_bit)) {
			ret = i2c_smbus_write_byte_data(ts->pmu, ts->pmu_reg, ret | ts->pmu_bit);
			if (ret)
				return ret;
			dev_info(dev, "touch analog LDO on\n");
			usleep_range(1000, 2000);
		}
	}
	if (ts->smc) {
		ret = apple_smc_write_u32(ts->smc, ts->smc_key, ts->smc_val | 1);
		if (ret)
			return ret;
		msleep(10);
	}
	return 0;
}

static int boot(struct ahtouch *ts)
{
	struct device *dev = &ts->spi->dev;
	int i, ret;

	ret = power_on(ts);
	if (ret) {
		dev_err(dev, "power on: %d\n", ret);
		return ret;
	}
	/* a firmware still running from before: an all-zero packet sends it back */
	for (i = 0; i < 3 && !hbpp_check(ts); i++) {
		memset(ts->tx, 0, HS_XFER);
		xfer(ts, HS_XFER);
		msleep(20);
	}
	if (!hbpp_check(ts)) {
		dev_err(dev, "no HBPP bootloader\n");
		return -ENODEV;
	}
	ret = hbpp_load(ts);
	if (ret)
		return ret;
	msleep(40);
	memset(ts->last, 0, HS_XFER);
	ts->fails = 0;
	dev_info(dev, "firmware running\n");
	return 0;
}

static int ahtouch_thread(void *data)
{
	struct ahtouch *ts = data;
	unsigned long active_until = 0;

	while (!kthread_should_stop()) {
		if (boot(ts)) {
			msleep_interruptible(5000);
			continue;
		}
		while (!kthread_should_stop() && ts->fails < 20) {
			if (hs_poll(ts))
				active_until = jiffies + msecs_to_jiffies(300);
			{
				unsigned int ms = time_before(jiffies, active_until) ?
						  poll_active_ms : poll_idle_ms;

				usleep_range(ms * 1000, ms * 1000 + 500);
			}
		}
		if (!kthread_should_stop())
			dev_warn(&ts->spi->dev, "firmware not answering, reloading\n");
	}
	return 0;
}

static int get_power(struct ahtouch *ts)
{
	struct device *dev = &ts->spi->dev;
	struct of_phandle_args args;
	int ret;

	ret = of_parse_phandle_with_fixed_args(dev->of_node, "apple,pmu-ldo", 2, 0, &args);
	if (!ret) {
		ts->pmu = of_find_i2c_device_by_node(args.np);
		of_node_put(args.np);
		if (!ts->pmu)
			return -EPROBE_DEFER;
		ts->pmu_reg = args.args[0];
		ts->pmu_bit = args.args[1];
	}
	ret = of_parse_phandle_with_fixed_args(dev->of_node, "apple,smc-function", 2, 0, &args);
	if (!ret) {
		struct platform_device *pdev = of_find_device_by_node(args.np);

		of_node_put(args.np);
		if (!pdev)
			return -EPROBE_DEFER;
		ts->smc = platform_get_drvdata(pdev);
		put_device(&pdev->dev);
		if (!ts->smc)
			return -EPROBE_DEFER;
		ts->smc_key = args.args[0];
		ts->smc_val = args.args[1];
	}
	return 0;
}

static int ahtouch_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct ahtouch *ts;
	int ret;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	ts->spi = spi;
	ts->tx = devm_kzalloc(dev, BUF_LEN, GFP_KERNEL);
	ts->rx = devm_kzalloc(dev, BUF_LEN, GFP_KERNEL);
	ts->last = devm_kzalloc(dev, BUF_LEN, GFP_KERNEL);
	ts->pkt = devm_kzalloc(dev, BUF_LEN, GFP_KERNEL);
	if (!ts->tx || !ts->rx || !ts->last || !ts->pkt)
		return -ENOMEM;
	spi_set_drvdata(spi, ts);

	ret = get_power(ts);
	if (ret)
		return ret;
	ret = device_property_read_string(dev, "firmware-name", &ts->fw_name);
	if (ret)
		return dev_err_probe(dev, ret, "no firmware-name\n");
	device_property_read_string(dev, "apple,calibration-name", &ts->cal_name);

	ts->input = devm_input_allocate_device(dev);
	if (!ts->input)
		return -ENOMEM;
	ts->input->name = "Apple iPhone touchscreen";
	ts->input->phys = "apple-hidspi-touch";
	ts->input->id.bustype = BUS_SPI;
	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, 0, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, 0, 0, 0);
	touchscreen_parse_properties(ts->input, true, &ts->props);
	input_abs_set_res(ts->input, ABS_MT_POSITION_X, 100);
	input_abs_set_res(ts->input, ABS_MT_POSITION_Y, 100);
	input_set_abs_params(ts->input, ABS_MT_WIDTH_MAJOR, 0, 65535, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_WIDTH_MINOR, 0, 65535, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 65535, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MINOR, 0, 65535, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_ORIENTATION, -32768, 32767, 0, 0);
	ret = input_mt_init_slots(ts->input, MAX_FINGERS, INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret)
		return ret;
	ret = input_register_device(ts->input);
	if (ret)
		return ret;

	ts->thread = kthread_run(ahtouch_thread, ts, "apple-touch");
	return PTR_ERR_OR_ZERO(ts->thread);
}

static void ahtouch_remove(struct spi_device *spi)
{
	struct ahtouch *ts = spi_get_drvdata(spi);

	kthread_stop(ts->thread);
	if (ts->pmu)
		put_device(&ts->pmu->dev);
}

static const struct of_device_id ahtouch_of_match[] = {
	{ .compatible = "apple,hbpp-multitouch" },
	{}
};
MODULE_DEVICE_TABLE(of, ahtouch_of_match);

static const struct spi_device_id ahtouch_id[] = {
	{ "hbpp-multitouch" },
	{}
};
MODULE_DEVICE_TABLE(spi, ahtouch_id);

static struct spi_driver ahtouch_driver = {
	.driver = {
		.name = "apple-hidspi-touch",
		.of_match_table = ahtouch_of_match,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table = ahtouch_id,
	.probe = ahtouch_probe,
	.remove = ahtouch_remove,
};
module_spi_driver(ahtouch_driver);

MODULE_DESCRIPTION("Apple HBPP/HIDSPI touchscreen (iPhone 11)");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE("apple/n104-mtfw.bin");
