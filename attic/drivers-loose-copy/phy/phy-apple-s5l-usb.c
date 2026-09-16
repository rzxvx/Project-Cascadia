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

/* ADT clock-ids: otgphyctrl uses id 5 (PHY block itself),
 * usb-complex uses id 292 (main USB clock) and clock-gates 87..91
 * (five gates that power the whole USB subsystem). */
#define USB_PHY_CLOCK_ID	5
#define USB_MAIN_CLOCK_ID	292
static const u8 usb_complex_gates[] = { 87, 88, 89, 90, 91 };

/* Tuning constants from ADT otgphyctrl node (device-mode values):
 *   uotgtune1-device = 0x549, uotgtune2-device = 0x2ff3
 *   ref-clock-sel    = 3
 *   usbhostset-en-incrx = 0xE0
 * Register offsets are guesses matching Samsung Exynos S5L OTG PHY layout
 * (which the Apple block is derived from). To be verified by XNU-hook dump. */
#define OPHY_UOTGTUNE1		0x30
#define OPHY_UOTGTUNE2		0x34

struct s5l_usbphy {
	void __iomem *base;
	void __iomem *pmgr;
	struct device *dev;
};

static void phy_dump_regs(struct s5l_usbphy *p, const char *tag)
{
	/* Layout learned empirically from the pre-init snapshot:
	 *   0x00 OPHYPWR    (was 0x6 from iBoot)
	 *   0x04 OPHYCLK    (was 0x1 = 24MHz)
	 *   0x08 ORSTCON    (was 0x0)
	 *   0x1C OPHYUNK1   (was 0x6)   -- probably uotgtune1
	 *   0x40 ???        (probe, might also be tuning)
	 *   0x44 OPHYUNK2   (was 0x2ff3 == ADT uotgtune2-device)
	 *   0x60 OPHYUNK4   (was 0x200)
	 * We keep 0x30 and 0x34 in the dump only to confirm they are unused
	 * (their pre-init values were 0). */
	pr_err("PHY dump %s: PWR=0x%08x CLK=0x%08x RST=0x%08x U1C=0x%08x R30=0x%08x R34=0x%08x R40=0x%08x U44=0x%08x U60=0x%08x\n",
	       tag,
	       readl(p->base + OPHYPWR),
	       readl(p->base + OPHYCLK),
	       readl(p->base + ORSTCON),
	       readl(p->base + OPHYUNK1),
	       readl(p->base + 0x30),
	       readl(p->base + 0x34),
	       readl(p->base + 0x40),
	       readl(p->base + OPHYUNK2),
	       readl(p->base + OPHYUNK4));
}

static int s5l_usbphy_init(struct phy *phy)
{
	struct s5l_usbphy *p = phy_get_drvdata(phy);
	unsigned i;
	u32 val;

	dev_info(p->dev, "PHY init start\n");

	/* -- pre-touch snapshot: what did iBoot leave us? -- */
	phy_dump_regs(p, "pre");

	/* Enable ALL usb-complex clock gates (87..91) + PHY gate 5 + main OTG clock 292.
	 * ADT lists these five together for usb-complex; missing any keeps DWC2 dead. */
	for (i = 0; i < ARRAY_SIZE(usb_complex_gates); i++) {
		unsigned g = usb_complex_gates[i];
		val = readl(p->pmgr + (PMGR_GATE_BASE - PMGR_BASE) + g * 4);
		pr_err("PHY gate %u pre=0x%08x\n", g, val);
		writel(PMGR_GATE_ON, p->pmgr + (PMGR_GATE_BASE - PMGR_BASE) + g * 4);
	}
	val = readl(p->pmgr + (PMGR_GATE_BASE - PMGR_BASE) + USB_PHY_CLOCK_ID * 4);
	pr_err("PHY gate %u(PHY) pre=0x%08x\n", USB_PHY_CLOCK_ID, val);
	writel(PMGR_GATE_ON, p->pmgr + (PMGR_GATE_BASE - PMGR_BASE) + USB_PHY_CLOCK_ID * 4);
	val = readl(p->pmgr + (PMGR_GATE_BASE - PMGR_BASE) + USB_MAIN_CLOCK_ID * 4);
	pr_err("PHY gate %u(MAIN) pre=0x%08x\n", USB_MAIN_CLOCK_ID, val);
	writel(PMGR_GATE_ON, p->pmgr + (PMGR_GATE_BASE - PMGR_BASE) + USB_MAIN_CLOCK_ID * 4);

	udelay(100);

	/* -- post-gate snapshot: are the regs alive now? -- */
	phy_dump_regs(p, "post-gates");

	/* DECISION: DO NOT overwrite iBoot-programmed PHY registers.
	 *
	 * Empirical dump on live hardware (2026-09-13):
	 *   U1C=0x06 (matches previous "OPHYUNK1_START"),
	 *   U44=0x2ff3 (== ADT uotgtune2-device — NOT 0xF8 as previous
	 *               driver comment claimed. Overwriting to 0xF8 breaks
	 *               PHY and panics the kernel),
	 *   U60=0x200 (matches previous "OPHYUNK4_START"),
	 *   PWR=0x06, CLK=0x01 (24MHz clksel already selected).
	 *
	 * So iBoot already leaves the PHY in the correct static state.
	 * Only thing this driver needs to do is: (a) make sure clock gates
	 * are ON (they weren't in the dump, but PHY regs were still alive),
	 * (b) release the soft reset, and (c) log what we saw so any residual
	 * problem is easy to spot in the UART log.
	 *
	 * Old comment claimed "OPHYUNK2=0xF8 is what iBoot's DFU state
	 * leaves set". That was wrong; the correct DFU state is 0x2ff3. */

	udelay(10);

	/* Toggle PHY soft reset — clears any pending state without touching
	 * the tuning constants. */
	writel(readl(p->base + ORSTCON) | ORSTCON_PHYSWRESET, p->base + ORSTCON);
	udelay(20);
	writel(readl(p->base + ORSTCON) & ~ORSTCON_PHYSWRESET, p->base + ORSTCON);
	udelay(1000);

	phy_dump_regs(p, "post-init");

	dev_info(p->dev, "PHY init done\n");
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
