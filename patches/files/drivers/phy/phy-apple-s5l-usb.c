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
#include <linux/regulator/consumer.h>

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

/* The host side: EHCI and its HSIC port, where the Wi-Fi chip is.
 *
 *   PMGR +0x1088   the USB 2.0 host block's power/clock register.  The ADT
 *                  calls it usb-complex's function-usb20_reset, 'ARST' 34:
 *                  reset ids index the gate array the way XNU reads it,
 *                  PMGR + 0x1000 + id * 4, ten ids above the gate numbering
 *                  (i2c0's reset, 70, is the register of its gate, 80).
 *                  iBoot leaves it off, and EHCI and OHCI then read as
 *                  nothing at all; switched on, EHCI answers (HCIVERSION
 *                  0x0100, three ports).  Measured 2026-09-26.
 *   PMGR +0x1140   gate 90, the one usb-complex gate iBoot leaves off.  It
 *                  was on when EHCI first answered; kept.
 *   usb-complex    +0 bit 2: HSIC enable.  AppleS5L8930XUSBArbitrator's
 *                  _configureHSIC ORs the ADT's usb_ctl (0x64) into this
 *                  register (iOS 6.1 kernelcache 0x808afffc); bits 5/6 there
 *                  are clock-off bits it clears again for active clients, so
 *                  bit 2 is what it leaves.  Bit 7 is the OTG PHY's own
 *                  (set on power-up, 0x344 on the same object) -- not ours.
 *   hsic-supply    the Wi-Fi chip's REG_ON, PMU GPIO3.  With all three on
 *                  and the port powered, PORTSC3 read 0x1803: connected.
 *
 * The regulator is taken when the host PHY powers on, not at probe: the PMU
 * sits behind I2C and may come later, and the OTG PHY -- the gadget, the
 * console, the network -- must not wait for it. */
#define PMGR_USB20_HOST		0x1088
#define PMGR_GATE_90		0x1140
#define USBCPLX_HSIC_EN		BIT(2)

struct s5l_usbphy {
	void __iomem *base;
	void __iomem *pmgr;
	void __iomem *usbcplx;		/* NULL without a second reg */
	struct regulator *hsic_supply;
	bool hsic_supply_on;
	struct regulator *lpo_supply;	/* the chip's 32 kHz sleep clock */
	bool lpo_supply_on;
	struct device *dev;
	struct phy *phys[2];		/* [0] OTG, [1] host/HSIC */
};

/* XNU's clock_gate_switch: request the state in bits 3:0, wait for bits 7:4
 * to follow.  Bit 8 is cleared with the request, as XNU does. */
static int s5l_pmgr_on(struct s5l_usbphy *p, u32 off)
{
	u32 v = readl(p->pmgr + off);
	int n;

	writel((v & ~0x10f) | 0xf, p->pmgr + off);
	for (n = 0; n < 1000; n++) {
		v = readl(p->pmgr + off);
		if (!((v ^ (v >> 4)) & 0xf))
			return 0;
		udelay(1);
	}
	dev_err(p->dev, "PMGR +0x%x stuck at 0x%08x\n", off, v);
	return -ETIMEDOUT;
}

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

static int s5l_hostphy_power_on(struct phy *phy)
{
	struct s5l_usbphy *p = phy_get_drvdata(phy);
	int ret;

	/* The 32 kHz clock first, the way a chip expects its sleep clock
	 * before it is let out of reset.  Optional like hsic-supply. */
	if (!p->lpo_supply) {
		struct regulator *r = devm_regulator_get_optional(p->dev, "lpo");

		if (IS_ERR(r)) {
			if (PTR_ERR(r) == -EPROBE_DEFER)
				return -EPROBE_DEFER;
		} else {
			p->lpo_supply = r;
		}
	}
	if (p->lpo_supply && !p->lpo_supply_on) {
		ret = regulator_enable(p->lpo_supply);
		if (ret)
			return ret;
		p->lpo_supply_on = true;
		msleep(10);
	}

	if (!p->hsic_supply) {
		struct regulator *r = devm_regulator_get_optional(p->dev, "hsic");

		if (IS_ERR(r)) {
			if (PTR_ERR(r) == -EPROBE_DEFER)
				return -EPROBE_DEFER;
			dev_warn(p->dev, "no hsic-supply (%ld): the HSIC device stays unpowered\n",
				 PTR_ERR(r));
		} else {
			p->hsic_supply = r;
		}
	}
	if (p->hsic_supply && !p->hsic_supply_on) {
		ret = regulator_enable(p->hsic_supply);
		if (ret)
			return ret;
		p->hsic_supply_on = true;
		msleep(10);
	}

	ret = s5l_pmgr_on(p, PMGR_USB20_HOST);
	if (ret)
		return ret;
	s5l_pmgr_on(p, PMGR_GATE_90);
	if (p->usbcplx)
		writel(readl(p->usbcplx) | USBCPLX_HSIC_EN, p->usbcplx);

	dev_info(p->dev, "host PHY on: PMGR +0x%x=0x%08x usb-complex=0x%08x\n",
		 PMGR_USB20_HOST, readl(p->pmgr + PMGR_USB20_HOST),
		 p->usbcplx ? readl(p->usbcplx) : 0);
	return 0;
}

static int s5l_hostphy_power_off(struct phy *phy)
{
	struct s5l_usbphy *p = phy_get_drvdata(phy);

	if (p->hsic_supply_on) {
		regulator_disable(p->hsic_supply);
		p->hsic_supply_on = false;
	}
	if (p->lpo_supply_on) {
		regulator_disable(p->lpo_supply);
		p->lpo_supply_on = false;
	}
	return 0;
}

static const struct phy_ops s5l_hostphy_ops = {
	.power_on  = s5l_hostphy_power_on,
	.power_off = s5l_hostphy_power_off,
	.owner     = THIS_MODULE,
};

/* <&otgphy 0> is the OTG PHY dwc2 has always had, <&otgphy 1> the host
 * side.  With #phy-cells = <0> there are no args, and that is the OTG PHY. */
static struct phy *s5l_usbphy_xlate(struct device *dev,
				    const struct of_phandle_args *args)
{
	struct s5l_usbphy *p = dev_get_drvdata(dev);
	unsigned int idx = args->args_count ? args->args[0] : 0;

	if (idx >= ARRAY_SIZE(p->phys))
		return ERR_PTR(-EINVAL);
	return p->phys[idx];
}

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

	/* usb-complex, for the HSIC enable bit: optional, the second reg. */
	if (platform_get_resource(pdev, IORESOURCE_MEM, 1)) {
		p->usbcplx = devm_platform_ioremap_resource(pdev, 1);
		if (IS_ERR(p->usbcplx))
			p->usbcplx = NULL;
	}
	platform_set_drvdata(pdev, p);

	phy = devm_phy_create(&pdev->dev, NULL, &s5l_usbphy_ops);
	if (IS_ERR(phy)) {
		dev_err(&pdev->dev, "failed to create PHY\n");
		return PTR_ERR(phy);
	}
	phy_set_drvdata(phy, p);
	p->phys[0] = phy;

	phy = devm_phy_create(&pdev->dev, NULL, &s5l_hostphy_ops);
	if (IS_ERR(phy)) {
		dev_err(&pdev->dev, "failed to create host PHY\n");
		return PTR_ERR(phy);
	}
	phy_set_drvdata(phy, p);
	p->phys[1] = phy;

	provider = devm_of_phy_provider_register(&pdev->dev, s5l_usbphy_xlate);
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
