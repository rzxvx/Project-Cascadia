# P105 DWC2 host profile: low-level and USB-core PHY ownership

## Result and limits

No new live-code reference-balance defect is established in the currently
exposed P105 DWC2 host probe/error/remove resource paths. Platform resource
enablement and USB-core HCD setup both acquire the same OTG PHY, but their
normal references are paired: init/power counts `0 -> 1 -> 2 -> 1 -> 0`.
The provider init callback executes once, not twice. The existing USB/PHY
reference-recovery patch keeps HCD error cleanup from consuming an unrelated
platform power reference. No runtime source is changed by this diagnostic.

This is offline software evidence. DWC2 host operation, external port routing,
VBUS, accessory identification, PHY tuning and attached-device enumeration
remain unvalidated. A successful forced-host kernel build does not establish
these hardware functions. EHCI/HSIC findings belong to PHY index 1 and cannot
be transferred to DWC2's PHY index 0 simply because both share one provider
structure.

## Actual profile and binding

The generated host profile retains `phys = <&otgphy 0>` and
`phy-names = "usb2-phy"` at `usb@6100000`; only its `dr_mode` is `host`.
EHCI at `usb@6400000` instead uses index 1. The reviewed merged host config has
`CONFIG_USB_DWC2_HOST=y`, `CONFIG_PM=y`, and no `CONFIG_SUSPEND` or
`CONFIG_HIBERNATION`.

The actual P105 DWC2 node does not declare an `otg` or `utmi` clock, a `vbus`
supply, or `vusb_d`/`vusb_a` supplies. The two DWC2 bulk supply names are the
upstream digital/analog core supplies; they are not an accessory VBUS source.
Without corresponding supply mappings, regulator-full-constraints permits
dummy bulk regulators. That source behavior is a scope argument, not a live
measurement or proof that the physical rails are controlled by this driver.

The P105 OTG PHY ops have `.init = s5l_usbphy_init` and the success-only
`.exit = s5l_usbphy_exit`, with no `.power_on`, `.power_off` or `.set_mode`.
Its init enables the existing OTG PMGR gate and respects the gate failure
before reset; it does not enable HSIC supplies. P105's HSIC supplies and PMGR
EHCI/OHCI handling are in the distinct host-PHY ops. No `phy-supply` is present
on the PHY node. PHY runtime PM is enabled by core creation only if its parent
already has runtime PM enabled; the current S5L PHY probe does not enable it.
The P105-shaped fixture explicitly uses no parent regulator and disabled PHY
runtime PM, while separate generic clock tests use synthetic optional clocks.

Local source boundaries: [P105 DT](../../dts/p105ap.dts),
[host-profile generator](../../scripts/prepare-usb-host.py), and
[S5L PHY provider](../../patches/files/drivers/phy/phy-apple-s5l-usb.c).

## Resource call order and cleanup

The actual DWC2 platform functions execute:

```text
lowlevel enable
  bulk supplies enable
  optional utmi clock enable
  optional otg clock enable
  phy_init
  phy_power_on
  ll_hw_enabled = true on success

HCD setup: usb_add_hcd PHY block
  usb_phy_roothub_alloc from same controller DT
  roothub init (second init reference)
  roothub set_mode (no power reference acquired by mode)
  roothub power_on (second power reference)

HCD remove / downstream HCD failure
  roothub power_off
  roothub exit

platform remove / probe error
  if ll_hw_enabled: lowlevel disable
    phy_power_off
    phy_exit
    optional otg clock disable
    optional utmi clock disable
    bulk supplies disable
    ll_hw_enabled = false only on success
```

A bulk-enable error acquires nothing. A failed UTMI clock enable disables only
bulk supplies. A failed OTG clock enable disables the newly enabled UTMI clock
and bulk supplies. Failed PHY init unwinds both optional clocks and supplies;
no init/power reference was acquired. Failed power-on calls `phy_exit()` before
that unwind. In P105's current ops, exit cannot fail and power-on has no
provider callback; generic regulator/PM error possibilities remain separate.

Full DWC2 probe source inspection follows failures in dr-mode checking,
GSNPSID/version, core reset, hardware parameters, IRQ acquisition, IRQ request,
parameter setup, DRD and HCD initialization to the common resource-disable
label. DWC2 remove stops the HCD before low-level resource shutdown. The actual
HCD PHY setup/error-tail and remove-PHY block are compiled verbatim in the
fixture; unrelated HCD allocations, controller reset, bus registration and
interrupt scheduling are not executed. No full controller probe is claimed.

## Historical shared-owner control and exposure

The exact original Linux `usb_phy_roothub_set_mode()` error path powers off
previously processed list entries even though it acquired no power references.
A two-entry synthetic root pointing twice at one PHY, with the second mode
request failing, reproduces consumption of DWC2's separate platform power
reference. Current set-mode code leaves that reference owned and lets subsequent
HCD cleanup release only its own initialized entries. The historical control
uses the exact original set-mode function surrounded by the current generic
core/wrapper; it is not represented as the entire original kernel.

This failure injection is not exposed by the actual single-PHY P105 DWC2 DT,
and P105 OTG ops have no failing mode callback. It is supplementary shared-owner
coverage of the already reviewed generic patch, not a new P105 hardware issue.
The platform gate-init failure is separately injectable before successful
initialization, matching the real init callback's newly checked return path.

## Scoped generic bulk-disable limitation

After successful PHY power-off/exit and optional clock disable, an injected
bulk-disable failure leaves `ll_hw_enabled=true`, although PHY/clock references
have already been released. In the fixture, the failed bulk provider retains
its reference. A second low-level disable encounters the current generic
core's zero-count guard and returns `-EINVAL` before reaching the retained
bulk reference. Returning success or forcibly decrementing the PHY count
would conceal ownership loss, not recover the supplies.

This is a generic consumer-state limitation, not a demonstrated P105 rail
failure: the actual node supplies are unmapped/dummy, both optional clock
pointers are absent, and the OTG PHY has no failing power-off/exit callback.
A future fix needs separate per-stage consumer ownership and explicit handling
of bulk rollback/partial-release errors. The fixture models a failure before
bulk reference release; real regulator-bulk rollback can itself fail and is
not proved atomic by this mock. No global core or P105 power-value change is
justified by that generic fixture.

Likewise, platform remove and probe error discard low-level-disable errors;
retained state can be lost with devres teardown. This deserves separate
consumer repair if failing resources are added, but no new runtime change is
introduced here. Suspend/resume is out of the active profile; enabling it would
require its distinct low-level flag/error semantics to be audited first.

## Pinned inputs and reproduce

[The diagnostic fixture](../../tests/test-dwc2-phy-lifecycle.py) reads original
DWC2 `platform.c` at Linux commit
`adc218676eef25575469234709c2d87185ca223a`, SHA256
`f1f89b559a774b10f02e1a726f0d4de8056e69b05786736e9d67b67500cd45a6`.
The local reviewed platform bytes match this original source; no platform
candidate patch is applied. Generic PHY, USB PHY and HCD candidate source is
reconstructed from pinned original bytes plus the reviewed local `0002` patch
by the existing helper. Normal balance and optional-clock cases also execute
the exact historical generic power-count functions. All source downloads and
production mutations are excluded.

```sh
python3 tests/test-dwc2-phy-lifecycle.py -v
```

Eight unittest methods passed on the local host and in the existing Ubuntu 22
image, with no skips. They compile actual low-level resource functions, current
PHY core/wrapper functions, actual HCD PHY excerpts and the actual success-only
S5L OTG exit callback. Only the provider-init fault oracle, lookups, containing
structures, regulator/clock/PM backends and ownership locks are simulated.
Provider init counts do not imply execution of the actual PMGR/reset/MMIO
sequence. No PHY or USB hardware access exists. C compilation uses warnings as
errors except the provider stub's existing unused parameter, plus UBSan.

Primary code:
[DWC2 platform](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/dwc2/platform.c),
[DWC2 HCD](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/dwc2/hcd.c),
[USB-core HCD](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/core/hcd.c),
[generic PHY core](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/phy/phy-core.c).
