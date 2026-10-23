# P105 host research: DWC2 and EHCI/OHCI are different controller paths

Research date: 2026-10-11. Reviewed local integration
`55ff2b5cf7ed266ee91a8a824b5a997784bf850c`, Linux v6.12
`adc218676eef25575469234709c2d87185ca223a`, and the pinned P105 iOS 8.4.1
12H321 inputs documented in [USB host research](p105-usb-host.md).
This is source/artifact inspection only, with no device access or new runtime
implementation. [EHCI firmware evidence](p105-usb-ehci.md) is not a DWC2
initialization recipe.

## Concrete identity and register boundaries

The actual ADT `usb-complex` child range maps child 0 to arm-io `0x06100000`.
The Linux `/soc` range adds physical `0x30000000`. Controller identities are
therefore separate even though they belong to the same USB complex:

| Block | Physical window | ADT interrupt | Linux consumer / PHY index |
|---|---|---|---|
| Synopsys USB device/OTG | `0x36100000`, size `0x10000` | 11 | `snps,dwc2`, `otgphy` index 0 |
| EHCI | `0x36400000`, size `0x10000` | 12 | `generic-ehci`, `otgphy` index 1 |
| OHCI0 | `0x36500000`, size `0x10000` | 13 | `generic-ohci`, currently disabled |
| Shared PHY register window | `0x36000000` | Separate provider | Both indices are software outputs of one driver |
| USB-complex control window | `0x3f108000` | Not a host port register | Shared clock/mux controls |

Linux 6.12's DWC2 register definitions identify `GSNPSID` at core offset
`0x40`, `GHWCFG2` at `0x48`, `GUSBCFG` at `0x0c`, and host `HPRT0` at
`0x440`. On this mapped node their respective physical addresses are
`0x36100040`, `0x36100048`, `0x3610000c`, and `0x36100440`. These are
**address derivations, not fresh register reads**.

EHCI capability registers have their own `CAPLENGTH`, `HCIVERSION` and
`HCSPARAMS`. The operational base is capability base plus `CAPLENGTH`;
its `PORTSC[n]` is operational offset `0x44 + 4*n`. The audited Apple EHCI
port mapping uses this layout. It is unrelated to DWC2 offset `0x440`.
OHCI uses yet another register layout. The shared numerical power bit
`1 << 12` in DWC2 `HPRT0` and EHCI `PORTSC` does not make those registers
interchangeable or establish the same electrical provider.

Primary pinned sources:
[DWC2 hw.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/dwc2/hw.h),
[EHCI register definitions](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/include/linux/usb/ehci_def.h).

## What the compiled host experiment actually contains

`scripts/prepare-usb-host.py` changes the DWC2 node's `dr_mode` from
`peripheral` to `host`; it does not change EHCI/OHCI nodes, the PHY provider,
USB-complex routing or an external power provider. The generated profile
removes DWC2 peripheral/dual-role and the gadget, and adds HID/storage support.

Inspection of the **compiled DTB**, not just its source, confirms:

- DWC2: physical `0x36100000`, IRQ 11, `phys = <phandle 0>`, `dr_mode = host`.
- EHCI: physical `0x36400000`, IRQ 12, `phys = <phandle 1>`, HSIC mask 4.
- OHCI0: physical `0x36500000`, IRQ 13, `status = disabled`, no PHY reference.
- Neither DWC2 nor EHCI acquires a newly invented `vbus-supply` property.

The merged `.config` has DWC2 host and EHCI platform built in. It **also**
has `USB_OHCI_HCD=y` and `USB_OHCI_HCD_PLATFORM=y` inherited from
`multi_v7_defconfig`. The P105 fragment lacking explicit OHCI selections
must not be described as proof that the compiled image lacks OHCI code.
Conversely, compiled code cannot enable a disabled DT node or give that
node the missing PHY/power lifecycle. “Host-only” describes the DWC2 role
choice; it does not mean only one host-controller implementation is compiled.

Audited artifacts under ignored `build/usb-host-research/kernel-output/`:

| File | SHA-256 at inspection |
|---|---|
| `.config` | `aaa667a691be21d36920956c52c9badbbdd2c76d57adc5f7ec97766991200da2` |
| `p105ap-usb-host.dtb` | `85a2d0ae4262c0f0e35891a12e9303f22242add6053abc4ec4e7c6b80fc3b5db` |

The DTB was decoded offline with `fdtget` in the existing network-disabled,
unprivileged build image; no new image, package or hardware was used. These
hashes identify an inspection snapshot, not a successful boot result.

## DWC2 role forcing is an IP operation, not Apple's platform route switch

In pinned Linux `platform.c`, probe initializes/powers the generic PHY,
checks the core, resets it and reads hardware parameters. For a host profile,
`params.c:dwc2_get_host_hwparams()` calls `dwc2_force_mode(hsotg, true)`
before reading host FIFO sizes. That function sets `GUSBCFG.FORCEHOSTMODE`
(bit 29), clears `FORCEDEVMODE` (bit 30) and waits for the core mode.
The host case in `dwc2_force_dr_mode()` itself contains only the host-only
hardware delay: the earlier parameter acquisition is the relevant forcing
call. There is no missing force call established by reading that case alone.

These functions address DWC2 registers. They do not select USB-complex bit 0,
classify a Lightning accessory, authenticate it, request Apple's PHY host
tuning or enable a P105 external supply. Current PHY index 0 retains the
peripheral-tuned initialization and has no role-aware `set_mode` callback.
Index 1's supplies/gates support EHCI/HSIC; renaming it “host” does not make
it a generic DWC2 host-mode PHY transition.

DWC2 HCD's optional regulator helpers return success when no `vbus_supply`
exists. Its root-hub request may still set `HPRT0.PWR`. Hence success of the
software request is not a measured VBUS output. `dwc2_read_hprt0()` also
clears enable/change bits with mask `0x2e`; Apple's EHCI read helper masks
its separate RW1C bits with `0x2a`. Copying the EHCI callback or masks into
DWC2 would be incorrect.

Pinned primary sources:
[platform.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/dwc2/platform.c),
[params.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/dwc2/params.c),
[core.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/dwc2/core.c),
[HCD](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/dwc2/hcd.c),
[HPRT helper](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/dwc2/hcd.h).

## Which firmware results transfer, and which do not

| Established evidence | Applies to | Does not establish |
|---|---|---|
| DWC2 HNP/SRP, internal DMA, 14 host channels and hub support reported by upstream | Capability claim for the core at `0x36100000` | Physical host route, raw `GHWCFG2` verification on our unit, working host DMA/enumeration |
| ADT `num-of-eps=14` | Device endpoint count | Host channel count or 14 root ports; Linux decodes host channels separately as `1 + GHWCFG2[17:14]` |
| Apple host cable criteria 5, USB-complex control bit 0 set | Platform route selection; EHCI/OHCI0 removable-port handlers react to the same host cable objects | A DWC2 mode change or an external voltage output |
| EHCI external Port1, OHCI0 companion ID 0 | Apple's declared and instantiated removable host/companion path | That forcing DWC2 host recreates that path |
| EHCI Port3/type 3, HSIC mask 4 | Internal WLAN host path | External Port1 power or Lightning VBUS |
| PHY host tuning `0x54f/0x6df3` at shared offsets `+0x40/+0x44` | Same physical PHY provider; actual calls recovered from EHCI/OHCI startup | A complete safe DWC2 host transition, ownership or return-to-device sequence |
| EHCI `PORT_POWER`/wake callback and emulated writes | EHCI mapped `PORTSC` only | DWC2 `HPRT0` initialization, adapter boost or PMU switch control |

For EHCI, direct low/full-speed root devices normally involve a companion;
high-speed hubs can carry split transactions. DWC2 has its own single-root-port
HCD, host channels and hub/split machinery (`HCSPLT`), with no requirement
to transplant the EHCI companion sequence. Neither software architecture has
been validated on this P105 external connector. Fourteen channels are not
fourteen connector ports, and shared hardware capability does not select
Apple's board mux automatically.

The discrepancy is therefore resolved at the evidence level: the DWC2 build
tests software availability for a capable IP block; the recovered Apple
external-host sequence selects a different EHCI/OHCI path. Whether an
alternative DWC2 host route is physically available remains open. A future
implementation should first prove the connector/controller route and shared
PHY ownership, then the actual accessory power/conversion/isolation path.
No controller switch, runtime register write or boot approval follows from
this document.
