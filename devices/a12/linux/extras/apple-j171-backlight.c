// SPDX-License-Identifier: GPL-2.0-only
/* J171 inherited SPMI backlight: no panel reset or cold initialization.
 * Native 21B91 AppleSPMIBacklight writes LE11 to USID15, register0x1e00.
 * AppleSMCPMU pKW4(rLDl,0) supplies the separate enable0/1 operation.
 */
#include <linux/backlight.h>
#include <linux/device.h>
#include <linux/mfd/macsmc.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/spmi.h>

#define J171_DAC 0x1e00
#define J171_MASK 0x7ff

static bool allow_writes = true;
module_param(allow_writes, bool, 0600);
MODULE_PARM_DESC(allow_writes, "Permit brightness writes (default enabled; false for read-only diagnosis)");

struct j171_bl {
 struct regmap *map;
 struct apple_smc *smc;
 struct mutex lock;
 unsigned int boot_level;
 bool enabled; /* inherited-on assumption checked by nonzero DAC at probe */
};

static int j171_read(struct j171_bl *bl)
{
 u8 data[2];
 unsigned int value;
 int ret = regmap_bulk_read(bl->map, J171_DAC, data, sizeof(data));
 if (ret)
  return ret;
 value = data[0] | data[1] << 8;
 if (value & ~J171_MASK)
  return -ERANGE;
 return value;
}

static int j171_get(struct backlight_device *bd)
{
 struct j171_bl *bl = bl_get_data(bd);
 int ret;
 mutex_lock(&bl->lock);
 ret = bl->enabled ? j171_read(bl) : 0;
 mutex_unlock(&bl->lock);
 return ret;
}

static int j171_update(struct backlight_device *bd)
{
 struct j171_bl *bl = bl_get_data(bd);
 unsigned int level = backlight_get_brightness(bd);
 u8 data[2];
 int ret;
 if (!READ_ONCE(allow_writes))
  return -EACCES;
 if (level > bl->boot_level)
  return -ERANGE;
 mutex_lock(&bl->lock);
 if (!level) {
  /* Native BKLU skips zero: disable via the separate SMC enable path. */
  ret = apple_smc_write_u32(bl->smc, SMC_KEY(rLDl), 0);
  if (!ret)
   bl->enabled = false;
  goto out;
 }
 data[0] = level & 0xff;
 data[1] = level >> 8;
 ret = regmap_bulk_write(bl->map, J171_DAC, data, sizeof(data));
 if (ret)
  goto out;
 if (!bl->enabled) {
  ret = apple_smc_write_u32(bl->smc, SMC_KEY(rLDl), 1);
  if (ret)
   goto out;
  bl->enabled = true;
 }
 ret = j171_read(bl);
 if (ret >= 0)
  ret = ret == level ? 0 : -EIO;
out:
 mutex_unlock(&bl->lock);
 return ret;
}

static const struct backlight_ops j171_ops = {
 .get_brightness = j171_get,
 .update_status = j171_update,
};

static int j171_probe(struct platform_device *pdev)
{
 struct device *dev = &pdev->dev;
 struct apple_smc_key_info info;
 struct backlight_properties props = { .type = BACKLIGHT_RAW };
 struct backlight_device *bd;
 struct platform_device *smcdev;
 struct device_node *np;
 struct spmi_device *pmic;
 struct j171_bl *bl;
 int ret;
 if (!of_machine_is_compatible("apple,j171a"))
  return -ENODEV;
 if (!dev->parent || !dev->parent->of_node)
  return -ENODEV;
 pmic = spmi_find_device_by_of_node(dev->parent->of_node);
 if (!pmic)
  return -EPROBE_DEFER;
 ret = (&pmic->dev == dev->parent && pmic->usid == 0x0f) ? 0 : -ENODEV;
 spmi_device_put(pmic);
 if (ret)
  return ret;
 bl = devm_kzalloc(dev, sizeof(*bl), GFP_KERNEL);
 if (!bl)
  return -ENOMEM;
 mutex_init(&bl->lock);
 bl->map = dev_get_regmap(dev->parent, NULL);
 if (!bl->map)
  return -EPROBE_DEFER;
 np = of_parse_phandle(dev->of_node, "apple,smc", 0);
 if (!np)
  return -EINVAL;
 smcdev = of_find_device_by_node(np);
 of_node_put(np);
 if (!smcdev)
  return -EPROBE_DEFER;
 bl->smc = platform_get_drvdata(smcdev);
 if (!bl->smc) {
  put_device(&smcdev->dev);
  return -EPROBE_DEFER;
 }
 if (!device_link_add(dev, &smcdev->dev, DL_FLAG_AUTOREMOVE_CONSUMER)) {
  put_device(&smcdev->dev);
  return -ENOMEM;
 }
 put_device(&smcdev->dev);
 ret = apple_smc_get_key_info(bl->smc, SMC_KEY(rLDl), &info);
 if (ret < 0)
  return dev_err_probe(dev, ret, "rLDl key discovery failed\n");
 if (info.size != sizeof(u32) || !(info.flags & APPLE_SMC_WRITABLE))
  return dev_err_probe(dev, -EINVAL, "rLDl metadata size=%u flags=%#x\n", info.size, info.flags);
 ret = j171_read(bl);
 if (ret <= 0)
  return dev_err_probe(dev, ret ? ret : -ENODATA, "inherited DAC read/preflight failed\n");
 bl->boot_level = ret;
 bl->enabled = true;
 props.max_brightness = bl->boot_level;
 props.brightness = bl->boot_level;
 props.power = BACKLIGHT_POWER_ON;
 bd = devm_backlight_device_register(dev, "j171-backlight", dev,
                                    bl, &j171_ops, &props);
 if (IS_ERR(bd))
  return PTR_ERR(bd);
 /* Deliberately no update_status: registration must not change hardware. */
 dev_info(dev, "inherited DAC=%u; brightness cap=%u; writes=%d; no probe writes\n",
          bl->boot_level, bl->boot_level, allow_writes);
 return 0;
}

static const struct of_device_id j171_ids[] = {
 { .compatible = "cascadia,j171a-pmic-backlight" },
 { }
};
MODULE_DEVICE_TABLE(of, j171_ids);
static struct platform_driver j171_driver = {
 .probe = j171_probe,
 .driver = { .name = "j171-backlight", .of_match_table = j171_ids },
};
module_platform_driver(j171_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("J171 inherited SPMI/SMC backlight");
