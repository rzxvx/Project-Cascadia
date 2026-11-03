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
#define PMGR_GATE_ON		0xF

/* usb-complex's power-state registers.
 *
 * The ADT gives usb-complex `clock-gates = <88 89 90 91 87>`, and those ids do
 * NOT index the register array.  Every id has its own register index in the
 * pmgr node's `device-clocks` table, and the id-to-index difference is not a
 * constant -- it is 10 across ids 56..83 and something else either side, so
 * *any* `base + id * 4` formula is a shortcut that holds in one neighbourhood
 * and silently misses everywhere else.  USB is one of the places it misses.
 * scripts/adt-pmgr-map.py prints the table; docs/research/p105-pmgr-gates.md
 * has the story.
 *
 *   id 88  USB-OTG    0x3f101084   the gadget: the console and the network
 *   id 89  USB-EHCI   0x3f101088   the host side, the Wi-Fi chip behind it
 *   id 90  USB-OHCI0  0x3f101090   EHCI's companion
 *   id 91  USB-OHCI1  0x3f101094
 *   id 87  USB-PHY    --           index 0: no register, nothing to gate
 *
 * This used to be `0x3F101008 + id * 4`, which put ids 88..91 at 0x1168..
 * 0x1174 -- addresses with no register behind them, which swallow writes and
 * read back zero.  The boot log said so all along: `PHY gate 87/88/89
 * pre=0x00000000` for three gates iBoot leaves ON and which must read ~0x2ff.
 * None of the five was ever switched; USB worked because iBoot leaves USB-OTG
 * on for its own DFU.
 *
 * `clock-ids` is a different property in a different namespace -- a reference
 * to a clock source, not to a gate.  otgphyctrl's `clock-ids = <5>` is
 * PREDIV3-CLK (clock register 0x3f10001c); usb-complex's `clock-ids = <292>`
 * is not a device-clocks entry at all.  Writing a gate value at `base + 5 * 4`
 * and `base + 292 * 4`, as this driver did, hit PERFCNT's gate and an empty
 * address respectively.  Neither has anything to do with USB. */
#define PMGR_USB_OTG		0x1084
#define PMGR_USB_EHCI		0x1088
#define PMGR_USB_OHCI0		0x1090
#define PMGR_USB_OHCI1		0x1094

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
 *   PMGR +0x1088   USB-EHCI's power-state register (ADT clock-gates id 89).
 *                  iBoot leaves it off, and EHCI and OHCI then read as
 *                  nothing at all; switched on, EHCI answers (HCIVERSION
 *                  0x0100, three ports).  Measured 2026-09-26.  The register
 *                  is right; the reasoning that first reached it -- 'reset
 *                  ids sit ten above the gate numbering' -- was not, see the
 *                  device-clocks note above.
 *   PMGR +0x1090   USB-OHCI0 (id 90), EHCI's companion controller, the other
 *                  usb-complex gate iBoot leaves off.  This used to be
 *                  +0x1140, read as 'gate 90' through the old formula; that
 *                  register is MCA, an audio block, and switching it on is
 *                  what actually happened every time EHCI came up.
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

	writel((v & ~0x10f) | PMGR_GATE_ON, p->pmgr + off);
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

	dev_info(p->dev, "PHY init start\n");

	/* -- pre-touch snapshot: what did iBoot leave us? -- */
	phy_dump_regs(p, "pre");

	/* The gadget's own gate.  iBoot leaves USB-OTG on -- its DFU runs on it,
	 * which is how this port ever enumerated with every gate write landing
	 * in a hole -- so this is normally a no-op that confirms the state
	 * rather than changes it.  The host-side gates belong to the host PHY
	 * and are switched in s5l_hostphy_power_on(); OHCI1 nothing here uses.
	 *
	 * Logged by name and by register, because the old messages named ids
	 * that were not the ids being written. */
	dev_info(p->dev, "usb-complex pre: OTG(88)=0x%08x EHCI(89)=0x%08x OHCI0(90)=0x%08x OHCI1(91)=0x%08x\n",
		 readl(p->pmgr + PMGR_USB_OTG), readl(p->pmgr + PMGR_USB_EHCI),
		 readl(p->pmgr + PMGR_USB_OHCI0), readl(p->pmgr + PMGR_USB_OHCI1));

	s5l_pmgr_on(p, PMGR_USB_OTG);

	dev_info(p->dev, "usb-complex OTG(88) now 0x%08x\n",
		 readl(p->pmgr + PMGR_USB_OTG));

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

	ret = s5l_pmgr_on(p, PMGR_USB_EHCI);
	if (ret)
		return ret;
	s5l_pmgr_on(p, PMGR_USB_OHCI0);
	if (p->usbcplx)
		writel(readl(p->usbcplx) | USBCPLX_HSIC_EN, p->usbcplx);

	dev_info(p->dev, "host PHY on: EHCI(89)=0x%08x OHCI0(90)=0x%08x usb-complex=0x%08x\n",
		 readl(p->pmgr + PMGR_USB_EHCI), readl(p->pmgr + PMGR_USB_OHCI0),
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
