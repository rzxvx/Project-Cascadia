// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple S5L USB PHY driver (otgphyctrl,s5l8940x)
 * Register values taken from a live iBoot/DFU PHY dump on real hardware
 * (see docs/PHY-DUMP.md): OPHYPWR=0x6 OPHYCLK=0x1 UNK1_1C=0x6 UNK2_44=0xF8
 * UNK4_60=0x200 ORSTCON=0x0 -- these are the exact bits iBoot leaves set
 * while its own USB (DFU) is actively enumerated, i.e. known-working state.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>

#define OPHYPWR		0x00
#define OPHYCLK		0x04
#define ORSTCON	0x08
#define OPHYUNK1	0x1C
#define OPHYUNK2	0x44
#define OPHYUNK4	0x60

#define OPHYPWR_PLLPOWERDOWN		BIT(1)
#define OPHYPWR_XOPOWERDOWN		BIT(2)
#define OPHYCLK_CLKSEL_MASK		0x3
#define OPHYCLK_CLKSEL_24MHZ		0x1
#define ORSTCON_PHYSWRESET		BIT(0)

/* Values as observed live from iBoot's working PHY state */
#define OPHYUNK4_START			0x200
#define OPHYUNK1_START			0x6
#define OPHYUNK2_START			0xF8

#define PMGR_BASE		0x3F100000
#define PMGR_GATE_BASE		0x3F101008
#define PMGR_GATE_ON		0xF
#define USB_PHY_CLOCKGATE	5
#define USB_OTG_CLOCKGATE	292

struct s5l_usbphy {
	void __iomem *base;
	void __iomem *pmgr;
	struct device *dev;
};

static int s5l_usbphy_init(struct phy *phy)
{
	struct s5l_usbphy *p = phy_get_drvdata(phy);
	u32 val;

	dev_info(p->dev, "PHY init start\n");

	/* Enable clock gates */
	val = readl(p->pmgr + (PMGR_GATE_BASE - PMGR_BASE) + USB_PHY_CLOCKGATE * 4);
	dev_info(p->dev, "PHY clockgate before: 0x%08x\n", val);
	writel(PMGR_GATE_ON, p->pmgr + (PMGR_GATE_BASE - PMGR_BASE) + USB_PHY_CLOCKGATE * 4);

	val = readl(p->pmgr + (PMGR_GATE_BASE - PMGR_BASE) + USB_OTG_CLOCKGATE * 4);
	dev_info(p->dev, "OTG clockgate before: 0x%08x\n", val);
	writel(PMGR_GATE_ON, p->pmgr + (PMGR_GATE_BASE - PMGR_BASE) + USB_OTG_CLOCKGATE * 4);

	/* Power on PHY - values matched to live-working iBoot state */
	writel(OPHYUNK4_START, p->base + OPHYUNK4);
	writel(OPHYPWR_PLLPOWERDOWN | OPHYPWR_XOPOWERDOWN, p->base + OPHYPWR);
	writel(OPHYUNK1_START, p->base + OPHYUNK1);
	writel(OPHYUNK2_START, p->base + OPHYUNK2);
	udelay(10);

	/* Select clock - iBoot uses 24MHz, not "OTHER" */
	writel((readl(p->base + OPHYCLK) & ~OPHYCLK_CLKSEL_MASK) |
	       OPHYCLK_CLKSEL_24MHZ, p->base + OPHYCLK);

	/* Reset PHY */
	writel(readl(p->base + ORSTCON) | ORSTCON_PHYSWRESET, p->base + ORSTCON);
	udelay(20);
	writel(readl(p->base + ORSTCON) & ~ORSTCON_PHYSWRESET, p->base + ORSTCON);
	udelay(1000);

	dev_info(p->dev, "PHY init done, OPHYPWR=0x%08x OPHYCLK=0x%08x\n",
		 readl(p->base + OPHYPWR), readl(p->base + OPHYCLK));

	return 0;
}

static int s5l_usbphy_exit(struct phy *phy)
{
	return 0;
}

static const struct phy_ops s5l_usbphy_ops = {
	.init  = s5l_usbphy_init,
	.exit  = s5l_usbphy_exit,
	.owner = THIS_MODULE,
};

static int s5l_usbphy_probe(struct platform_device *pdev)
{
	struct s5l_usbphy *p;
	struct phy *phy;
	struct phy_provider *provider;

	dev_info(&pdev->dev, "Apple S5L USB PHY probe\n");

	p = devm_kzalloc(&pdev->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	p->dev = &pdev->dev;

	p->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(p->base)) {
		dev_err(&pdev->dev, "failed to ioremap PHY regs\n");
		return PTR_ERR(p->base);
	}

	p->pmgr = devm_ioremap(&pdev->dev, PMGR_BASE, 0x2000);
	if (!p->pmgr) {
		dev_err(&pdev->dev, "failed to ioremap PMGR\n");
		return -ENOMEM;
	}

	phy = devm_phy_create(&pdev->dev, NULL, &s5l_usbphy_ops);
	if (IS_ERR(phy)) {
		dev_err(&pdev->dev, "failed to create PHY\n");
		return PTR_ERR(phy);
	}

	phy_set_drvdata(phy, p);

	provider = devm_of_phy_provider_register(&pdev->dev, of_phy_simple_xlate);
	if (IS_ERR(provider))
		return PTR_ERR(provider);

	dev_info(&pdev->dev, "Apple S5L USB PHY registered\n");
	return 0;
}

static const struct of_device_id s5l_usbphy_of_match[] = {
	{ .compatible = "apple,s5l8940x-otgphy" },
	{ .compatible = "apple,s5l8930x-otgphy" },
	{ },
};
MODULE_DEVICE_TABLE(of, s5l_usbphy_of_match);

static struct platform_driver s5l_usbphy_driver = {
	.probe  = s5l_usbphy_probe,
	.driver = {
		.name           = "phy-apple-s5l-usb",
		.of_match_table = s5l_usbphy_of_match,
	},
};
module_platform_driver(s5l_usbphy_driver);

MODULE_DESCRIPTION("Apple S5L USB PHY driver");
MODULE_LICENSE("GPL v2");
