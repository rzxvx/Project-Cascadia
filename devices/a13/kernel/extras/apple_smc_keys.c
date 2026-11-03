// SPDX-License-Identifier: GPL-2.0
/*
 * Read and write raw SMC keys from debugfs, for bring-up: the ADT's
 * `function-*` entries on smc-pmu are SMC key writes (the touch controller's
 * clock and LDO), and they have to be read before they are written.
 *
 *   echo gP00 > /sys/kernel/debug/apple_smc_keys/key    select a key
 *   cat /sys/kernel/debug/apple_smc_keys/key            gP00 type ui32 size 4 flags 0xd0 value 01000000
 *   echo 'gP00 01000001' > /sys/kernel/debug/apple_smc_keys/key
 *                                                       write the bytes as given (hex, key's order)
 *   echo 'rBK3 ? 00000000' > /sys/kernel/debug/apple_smc_keys/key
 *                                                       write-then-read (function keys, the ADT's
 *                                                       pKR4); cat shows the answer
 */

#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/hex.h>
#include <linux/mfd/macsmc.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>

static struct apple_smc *smc;
static struct dentry *dir;
static DEFINE_MUTEX(lock);
static smc_key cur;
static u8 rw_in[32], rw_out[32];
static size_t rw_inlen;
static int rw_ret = 1;	/* 1: no rw done for this key */

/* The SMC may finish probing after us (RTKit boot), so find it on first use. */
static int get_smc(void)
{
	struct device_node *np;
	struct platform_device *pdev;

	if (smc)
		return 0;
	np = of_find_compatible_node(NULL, NULL, "apple,smc");
	if (!np)
		np = of_find_compatible_node(NULL, NULL, "apple,t8103-smc");
	if (!np)
		return -ENODEV;
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return -ENODEV;
	smc = platform_get_drvdata(pdev);
	put_device(&pdev->dev);
	return smc ? 0 : -EAGAIN;
}

static ssize_t key_read(struct file *f, char __user *ubuf, size_t n, loff_t *ppos)
{
	struct apple_smc_key_info info;
	u8 val[32];
	char out[160];
	int len, ret, i;

	mutex_lock(&lock);
	if (get_smc()) {
		len = scnprintf(out, sizeof(out), "no SMC\n");
		goto done;
	}
	if (!cur) {
		len = scnprintf(out, sizeof(out), "no key selected\n");
		goto done;
	}
	ret = apple_smc_get_key_info(smc, cur, &info);
	if (ret) {
		len = scnprintf(out, sizeof(out), "%p4ch: info error %d\n", &cur, ret);
		goto done;
	}
	len = scnprintf(out, sizeof(out), "%p4ch type %p4ch size %u flags %#x ",
			&cur, &info.type_code, info.size, info.flags);
	if (rw_ret <= 0) {
		if (rw_ret < 0)
			len += scnprintf(out + len, sizeof(out) - len, "rw %*phN: error %d\n",
					 (int)rw_inlen, rw_in, rw_ret);
		else
			len += scnprintf(out + len, sizeof(out) - len, "rw %*phN -> %*phN\n",
					 (int)rw_inlen, rw_in,
					 (int)min_t(size_t, info.size, sizeof(rw_out)), rw_out);
		goto done;
	}
	len += scnprintf(out + len, sizeof(out) - len, "value ");
	ret = apple_smc_read(smc, cur, val, min_t(size_t, info.size, sizeof(val)));
	if (ret < 0) {
		len += scnprintf(out + len, sizeof(out) - len, "error %d\n", ret);
		goto done;
	}
	for (i = 0; i < min_t(int, info.size, sizeof(val)); i++)
		len += scnprintf(out + len, sizeof(out) - len, "%02x", val[i]);
	len += scnprintf(out + len, sizeof(out) - len, "\n");
done:
	mutex_unlock(&lock);
	return simple_read_from_buffer(ubuf, n, ppos, out, len);
}

static ssize_t key_write(struct file *f, const char __user *ubuf, size_t n, loff_t *ppos)
{
	char buf[96], *p;
	u8 val[32];
	size_t vlen = 0;
	int ret;

	if (n >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, n))
		return -EFAULT;
	buf[n] = 0;
	p = strim(buf);
	if (strlen(p) < 4)
		return -EINVAL;

	mutex_lock(&lock);
	ret = get_smc();
	if (ret) {
		mutex_unlock(&lock);
		return ret;
	}
	cur = _SMC_KEY(p);
	rw_ret = 1;
	p = skip_spaces(p + 4);
	if (*p == '?') {
		struct apple_smc_key_info info;

		p = skip_spaces(p + 1);
		rw_inlen = 0;
		while (isxdigit(p[0]) && isxdigit(p[1]) && rw_inlen < sizeof(rw_in)) {
			rw_in[rw_inlen++] = (hex_to_bin(p[0]) << 4) | hex_to_bin(p[1]);
			p = skip_spaces(p + 2);
		}
		ret = apple_smc_get_key_info(smc, cur, &info);
		if (!ret)
			ret = apple_smc_rw(smc, cur, rw_in, rw_inlen, rw_out,
					   min_t(size_t, info.size, sizeof(rw_out)));
		rw_ret = ret < 0 ? ret : 0;
		mutex_unlock(&lock);
		return n;
	}
	while (isxdigit(p[0]) && isxdigit(p[1]) && vlen < sizeof(val)) {
		val[vlen++] = (hex_to_bin(p[0]) << 4) | hex_to_bin(p[1]);
		p += 2;
		p = skip_spaces(p);
	}
	ret = 0;
	if (vlen) {
		ret = apple_smc_write(smc, cur, val, vlen);
		pr_info("apple_smc_keys: write %p4ch %*phN -> %d\n", &cur, (int)vlen, val, ret);
	}
	mutex_unlock(&lock);
	return ret ? ret : n;
}

static const struct file_operations key_fops = {
	.read = key_read,
	.write = key_write,
	.llseek = default_llseek,
};

static int __init apple_smc_keys_init(void)
{
	dir = debugfs_create_dir("apple_smc_keys", NULL);
	debugfs_create_file("key", 0600, dir, NULL, &key_fops);
	return 0;
}
late_initcall(apple_smc_keys_init);

MODULE_DESCRIPTION("Raw SMC key access in debugfs (bring-up)");
MODULE_LICENSE("GPL");
