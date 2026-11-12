# Merge status — XR, iPad 8 and XS Max

The kieroid XS Max series has been integrated into the per-device profile
interface. XR is still default; `xsmax` and `A12_BOARD=d331p` select XS Max.
Conflicting selectors fail closed. Board-specific m1n1, DTB, firmware and
initramfs outputs are isolated; the Linux kernel source is shared.

This merge includes his refreshed AGX patch, not the later tested iPad GPU
experiment under `/tmp`. That experiment is a separate integration step.
No new hardware boot has validated the combined source yet.

The earlier bring-up record is in [HISTORY.md](HISTORY.md). The retained
XR/iPad port plan follows; its older testing statements are not evidence
that this newly merged tree has been hardware tested.

---

# Linux on iPhone XR (N841AP/T8020) — port status and plan

> **Historical engineering log:** current XR/iPad preparation and boot instructions are in [QUICKSTART.md](QUICKSTART.md). Older paths and commands below document experiments, not the current interface.


> Fork port note (2026-11-04): the Bluetooth GPIO/UART register logging and
> failed no-flow experiment described below are historical diagnosis only.
> Their patches and enabling DeviceTree properties are not in this branch.
> Working UART1 pinmux/RTS-CTS, SMC power, wake GPIO and firmware are in the
> retained source series; obsolete diagnostic-only variants are excluded.

Last updated: 2026-11-04

## Current validated state

The source-built, SysCfg-injected N841 payload boots tethered from DFU into a
Debian 13.7 NFS root with XFCE. The iBoot framebuffer, multitouch, USB
ACM/ECM, SSH, BCM4357 Wi-Fi firmware and BCM4349B1 UART Bluetooth HCD are
working. `boot-and-load-xr.sh` completed the full sequence automatically;
Wi-Fi exposed `wlp1s0`, Bluetooth HCD build 1072 powered `hci0`, and both
NetworkManager and `bluetooth.service` were active. The current ticketed
payload is local/ignored and has SHA256
`e9618a2a70fc49f9d276c679fa4751e1f041c224508527f89adc010a534f271d`.
Native display reinitialization/backlight, polished touch UI mapping, radio
pairing/profile endurance and untethered boot remain future work. Sections
below preserve the dated bring-up record and should not be read as the latest
status where they conflict with this summary.

## Goal

Bring the iPhone XR to the same practical Linux milestone as Cascadia's
iPhone 11 work, concentrating on:

1. a stable framebuffer/display;
2. working multitouch;
3. BCM4357-family Wi-Fi (`yodak`);
4. N88 Bluetooth over UART.

The boot remains tethered and volatile. Do not write to the phone's NVMe.
The recovery environment is an initramfs in RAM; the tested desktop uses a
host-exported Debian NFS root.

## Important hardware differences from iPhone 11

The A13/N104 implementation is a reference, not a drop-in DT or driver set.

| Function | iPhone 11 (N104/T8030) | iPhone XR (N841/T8020) |
|---|---|---|
| CPU | Lightning/Thunder | Vortex/Tempest |
| USB device | DWC3/AUSB | DWC2 plus T8020 USB complex/DART |
| Wi-Fi | BCM4378 on PCIe | `wlan-pcie,bcm4357`, module `yodak`, on PCIe port 2 |
| Bluetooth | BCM4378 PCIe function | `bluetooth,n88` on UART1 at 3 Mbit/s |
| Display PMU | Chestnut | Chestnut on I2C2 @ 0x27, not the XS Max Acorn PMU |
| Touch transport | Apple HID-over-SPI | Apple HID-over-SPI, older `spi-1,samsung` block |
| Display | T8030 pipe, Pinot panel | H11P/T8020 pipe, Pinot panel confirmed by N841 ADT |

## Verified milestones

### Boot chain

- [x] USBLiter8-pwned DFU accepts the patched XR iBSS.
- [x] iBSS enters recovery/yoloDFU and accepts `rdtr`, `rtsc`, and `rkrn`.
- [x] m1n1 runs on T8020 and hands off to a 16K-page arm64 Linux kernel.
- [x] Linux reaches `/init` and remains running.
- [x] The iBoot framebuffer is exposed by simpledrm as `/dev/fb0`; kernel text
      is visible on the XR screen.

Current volatile upload order:

```text
iBSS.linux.serial.raw
DeviceTree.img4                 (pristine N841 Apple ADT)
RestoreTrustCache.img4
m1n1-linux.img4                (m1n1 + Linux DTB + kernel + initramfs)
```

### m1n1 work completed

The A12 m1n1 patch set in `devices/a12/m1n1/` now provides:

- T8020/N841 platform recognition and Linux handoff;
- T8020 USB clock and power setup;
- DWC2 EP0 diagnostics used during bring-up;
- DWC2 DMA/cache/UTMI fixes;
- USB DART/remap setup;
- Vortex/Tempest MIDR handling and chicken bits;
- T8020 CPU voltage-rail initialization;
- T8020 PCIe initialization using live ADT tunables.

The current m1n1 artifact is:

```text
devices/a12/build/payload/m1n1-idevice.macho
SHA256 11efec7cd488ddc2d8fae36a41a28763c506d42455839f30ffb59224d5eed8b4
```

### Linux platform work completed

- [x] T8020 PMGR domains and six CPU descriptions.
- [x] AIC CPU-index correction.
- [x] T8020 AUSB PHY and DWC2 device mode.
- [x] USB DART with the T8020 mapper/remap behavior.
- [x] SART v1 and T8020 ANS2/NVMe detection.
- [x] T8020 PCIe controller and port-2 power/reset description.
- [x] A BCM4377-specific Bluetooth patch from the A12 reference is staged,
      but the N841 ADT proves it is **not the XR Bluetooth transport** and it
      must not be enabled for this board.
- [x] T8020 legacy SPI support is staged as a kernel patch.
- [x] H11P display and A12 multitouch drivers are staged as kernel patches.
- [x] simpledrm works on the iBoot-provided framebuffer.

Linux detects the Apple NVMe controller and its namespaces.  They are not a
Linux root and must remain unmounted during this bring-up.  Add an explicit
kernel/initramfs write guard before doing any storage experiments.

### USB development access

- [x] ConfigFS composite gadget: ACM (`ttyGS0`) plus ECM (`usb0`).
- [x] XR address `172.16.42.1/24`; host address `172.16.42.2/24`.
- [x] DHCP server in the initramfs.
- [x] Dropbear key-only SSH: `ssh root@172.16.42.1`.
- [x] ACM shell remains available independently of networking.
- [x] `usbwatch` detects a missing host and unbinds/rebinds the gadget.
- [x] Physical cable disconnect/reconnect was tested: USB, ping, and SSH all
      recovered without rebooting.

Current packaged Linux payload:

```text
devices/a12/boot/inputs/m1n1-linux.img4
SHA256 e9618a2a70fc49f9d276c679fa4751e1f041c224508527f89adc010a534f271d
```

## Current driver truth

The presence of a patch under `devices/a12/linux/patches/` does **not** mean that the
driver is enabled or working on N841.  The last verified kernel configuration
has:

```text
CONFIG_DRM_SIMPLEDRM=y
# CONFIG_DRM_APPLE_H11P is not set
# CONFIG_TOUCHSCREEN_APPLE_MT_SPI is not set
# CONFIG_BT_HCIBCM4377 is not set
```

The N841 board DTS currently enables only the serial node.  The shared T8020
DTS contains the display, DSI, I2C3, SPI1, PCIe and DART descriptions, but the
board-specific display/touch/wireless child nodes still have to be added and
validated against the XR ADT.  Its current comments describing port 2 as a
combined BCM4377 Wi-Fi/Bluetooth device came from the XS Max reference and are
wrong for N841; Wi-Fi is PCIe BCM4357-family and Bluetooth is on UART1.

## N841 ADT audit (2026-11-02)

The pristine `DeviceTree.n841ap.raw` was parsed with `ipsw dtree --json`.
These are board facts, not assumptions copied from another T8020 device:

- Display: `disp0,t8020`, Synopsys `mipi-dsim-1`, panel `lcd,pinot`.
- Display PMU: `display-pmu,chestnut`, I2C2 address `0x27`.
- Touch: SPI1 (`spi-1,samsung`), CS GPIO 67, reset GPIO 5, AOP interrupt
  GPIO 19, HBPP version `0x103`, image tag `wftm`, calibration `syscfg/MtCl`.
- Wi-Fi: PCIe port 2, compatible `wlan-pcie,bcm4357`, module instance
  `yodak`, calibration `syscfg/WCAL`, antenna SKU `syscfg/WSKU`.
- Wi-Fi control UART: UART2; REG_ON is an SMC/PMGR function and its interrupt
  GPIO is 22.
- Bluetooth: `bluetooth,n88` under UART1, 3,000,000 baud, power-enable via
  PMGR/SMC, wake GPIO 6, calibration `BTRx`, `BTTx`, and `BCAL`.

This changes the porting direction: the XS Max/Bad Apple BCM4377 PCIe
Bluetooth node and Acorn touch-power sequence cannot be copied into N841.

The matching iOS 18.7.10 (22H374) files already available locally include
`XRa1n/work/cfw/Firmware/N841_Multitouch.im4p`, so touch firmware conversion
does not require another full IPSW download.

## Remaining work: display

### What works now

- iBoot configures the panel before Linux starts.
- m1n1 passes the framebuffer address and format into the Linux DT.
- simpledrm keeps that scanout alive and provides `/dev/fb0`.
- This is enough for a framebuffer console and later a software-rendered UI.

### What is not yet working

- Native page flips through the T8020/H11P display pipe.
- XR panel/DSI description and brightness control.
- Backlight sysfs control.
- Reinitializing the display after it has been powered down.

### Next display steps

1. Translate the confirmed N841 `disp0`, `mipi-dsim`, `lcd,pinot`, Chestnut
   PMU and backlight properties into Linux bindings.
2. Add the XR Pinot panel endpoint under `&dsi` in `t8020-n841.dts`; reuse
   Cascadia's A13 Pinot knowledge where the protocol matches, not the XS Max
   `apple,d331p-summit` identity.
3. Enable `CONFIG_DRM_APPLE_H11P=y` while retaining simpledrm as fallback.
4. Boot with SSH and confirm takeover without blanking the panel.
5. Test XRGB8888 dumb buffers and page flips.
6. Add/validate DCS brightness (`0x51`) and expose backlight control.

Success criterion: a userspace framebuffer/DRM test can continuously redraw
and page-flip after the native driver takes over, with brightness adjustable.

## Remaining work: touchscreen

### Reusable work already present

- `linux-0009-apple-mt-spi.patch`: T8020 Apple multitouch/HIDSPI driver from
  the XS Max reference; transport/HBPP code is reusable, but its Acorn power
  sequence is not correct for the XR's Chestnut PMU.
- `linux-0010-spi-apple-legacy.patch`: polled mode for the old Samsung SPI
  block used by A12.
- Shared DTS nodes for I2C2, SPI1, AP GPIO and AOP GPIO.

### N841 work still required

**2026-11-02 controlled boot:** The BCM4357-enabled kernel and N841 DTB boot
Linux when the initramfs excludes the new Broadcom firmware files.  DCSD
confirms PCI endpoint `14e4:440a`, `brcmfmac` binding, `/init`, USB gadget,
and touch firmware running.  The driver currently fails at
`brcmf_chip_tcm_rambase: unknown chip: BCM4357/4` followed by
`RAM base not provided with ARM CR4 core` and probe error `-22`; fix chip
RAM-base handling before firmware loading can work.  Adding all `brcm/*`
files increased the initramfs from 5,386,240 to 6,423,552 bytes and causes
m1n1 to SError while decompressing it, before Linux runs.  The no-firmware
diagnostic IMG4 is `/tmp/xr-wifi-kernel-nofw-init.img4` (SHA256
`e364f751d7d7e7abd1d80fd66d0ceab9e202f0ad255581b8ecfcf29846f666d3`).
Do not replace the known-good touch baseline; keep testing one variable at a
time, then reintroduce firmware in smaller groups or another representation.

**Next candidate prepared, not yet phone-tested:** Broadcom's `bcmdhd`
reference driver puts BCM4357 in the BCM4347 group with CR4 TCM base
`0x170000`; patch 0016 adds that mapping to `brcmfmac`.  To avoid the
confirmed m1n1 decompression fault, the default XR initramfs omits Broadcom
files (5,386,240 bytes uncompressed, same as the boot-proven variant).
After Linux USB ECM/SSH is up, `devices/a12/boot/load-wifi-firmware.sh` copies the
matching yodak files into RAM and rebinds PCI function `0000:01:00.0`.
The candidate boot image is `devices/a12/boot/inputs/m1n1-linux.img4`, SHA256
`226a26e2b73b94911ca23e4eac183822f5ae5b828f0235746da7bee761995404`.
This isolates kernel RAM-base support from firmware delivery and preserves
the known-good touch payload.

**Phone result (2026-11-02):** IMG4 `226a26e2...` booted Linux with USB ECM,
SSH and touch intact.  The `0x170000` RAM-base patch passed: brcmfmac reaches
firmware request for BCM4357/4.  We copied the yodak firmware over USB SSH
into RAM, then learned that the extracted DRDO-u NVRAM must also be available
under the generic `brcmfmac4357b1-pcie.txt` fallback name.  With that name,
re-binding PCI `0000:01:00.0` initialized ThreadX v5.6, reported firmware
`13.10.706.0.0.0.88` (FWID `01-f4e64986`), and created `wlan0`.
`devices/a12/boot/load-wifi-firmware.sh` now performs that RAM-only transfer/alias
and rebind (default NVRAM variant `u`; `XR_BRCM_NVRAM_VARIANT=m` overrides).
Association, traffic and the correct phone-specific NVRAM/WSKU/WCAL remain
unverified.  Initial firmware-missing probes emit a kernel workqueue warning;
the successful rebind does not require a reboot.

**Wi-Fi radio test:** Transferred ARM64 `iw` plus its two `libnl` libraries to
the XR's RAM over USB SSH. `iw dev wlan0 info` succeeds (managed interface,
5 GHz channel visible). `iw dev wlan0 scan` exits 0 and discovers 6 BSS on
2.4 and 5 GHz (2412, 2437, 2457, 2462, 5200 MHz). Association, DHCP and
traffic are not yet tested; no network credentials were used or saved.

**Association and traffic verified (2026-11-02):** With a temporary WPA2
configuration supplied by the user (credential not recorded here), the XR
associated at 2.4 GHz, received DHCP address `192.168.1.11`, and passed
5/5 ICMP probes to the router and 5/5 to a public IP.  Host-to-XR SSH at
`192.168.1.11` worked, and XR-to-host probes passed 3/3.  This proves
bidirectional Wi-Fi and internet routing while the USB cable remained
attached; unplug/reconnect endurance and automatic boot-time setup are still
untested.  The NVRAM variant used was the extracted DRDO-u file.

**Cable-free syscfg/stability follow-up:** With only Wi-Fi SSH, read the
128 KiB NVMe namespace 3 after `blockdev --setro`; the private raw copy is
`/tmp/xr-syscfg.bin` (0600, do not commit or print identifiers).  Keys are
present: `WMac` 16 bytes, `WSKU` 16 (`NA` in bytes 8–9), `WCAL` 1888 bytes,
`BMac` 16, `BCAL` 3340, `MtCl` 2156.  The first 6 WMac bytes match the
active `wlan0` MAC.  Current DT placeholder antenna `R` is therefore wrong
for this phone; next package should inject WMac, WSKU=`NA`, WCAL from this
private syscfg, as the A13 packer does.  At 14 minutes uptime, Wi-Fi SSH
remained available without USB; 20/20 Wi-Fi pings and an 8 MiB SSH transfer
to `/dev/null` passed.  wlan0 counters: 6418 RX / 3724 TX, zero errors or
drops.  No post-association firmware timeout or disconnect was seen.  The
early firmware-missing/probe warnings preceded the successful RAM-only
rebind and are not a later stability failure.

**Offline next-boot package prepared:** `devices/a12/boot/package-linux.py` accepts
`XR_SYSCFG=/tmp/xr-syscfg.bin` and injects exactly WMac (first 6 bytes),
WSKU (`NA`), and WCAL into a temporary copy of the N841 DTB; it never prints
the MAC/calibration or copies the raw syscfg into the repo.  Embedded DTB
was extracted from the candidate IMG4 and independently checked against
syscfg: SKU, MAC and all 1888 calibration bytes match.  The Wi-Fi/SSH-proven
pre-injection IMG4 is preserved as
`devices/a12/boot/inputs/m1n1-linux-4357-baseline.img4` (SHA256
`226a26e2b73b94911ca23e4eac183822f5ae5b828f0235746da7bee761995404`).
New, **not yet phone-booted**, candidate is `devices/a12/boot/inputs/m1n1-linux.img4`
(SHA256 `df5feb361b552e71e54b3a6502240ecd1dbeaeb60363b6823cc72806690098b2`).

The N841 touch node is now under SPI1 and its Chestnut PMU is at I2C2 address
`0x27`. The matching `N841_Multitouch.im4p` personality `C1FC2,1` has been
converted to `n841-mtfw.bin` and `n841-mtcal.bin` and packaged in initramfs.
The built-in driver accepts the A12 HIDSPI report ID `0x44`.

First hardware test: Linux registered `Apple iPhone touchscreen`, then logged
`apple-hidspi-touch spi0.0: power on: -6`. The PMU had accidentally been
placed on I2C3; the pristine N841 ADT places Chestnut on **I2C2**. The
corrected payload was boot-tested successfully on 2026-11-02:
`m1n1-linux.img4` SHA256
`7a67ff84651a0b86b14f91535efce21cd15f87acf56e9f4418a9c54fb32fac12`.

Chestnut access now succeeds (`touch analog LDO on`), calibration loads as
2156 bytes, HBPP firmware starts (`firmware running`), and Linux exposes
`Apple iPhone touchscreen` as `/dev/input/event1`. A physical touch/swipe test
captured 16,560 bytes: 690 input events in 119 SYN frames, eight complete
contact lifecycles (`TRACKING_ID` 0 through 7, each followed by `-1`) and
matching `BTN_TOUCH` press/release events. Report-ID `0x44` parsing therefore
works. Observed coordinates were X 571..4214 and Y 6854..13354; the configured
range/orientation still needs a deliberate full-screen corner test before UI
use.

Success criterion: ten-finger events appear on a new `/dev/input/event*`, and
`evtest` reports correct coordinates and orientation across the full screen.

## Remaining work: Wi-Fi (BCM4357-family/yodak)

### Reusable work already present

- m1n1 initializes T8020 PCIe from the live ADT tunables.
- Linux PCIe port 2, PERST, REG_ON and `dart-apcie2` are described.
- The T8020 4K-DART protection workaround is staged.
- The N841 ADT identifies the device as `wlan-pcie,bcm4357`, module `yodak`.
- Bad Apple's BCM4377/geniek firmware mapping is not the XR mapping.

### N841 work still required

1. Confirm PCIe port 2 enumeration and record vendor/device/function IDs.
2. Add only the N841 Wi-Fi function under `&port02`; Bluetooth does not live
   on PCIe on this board.
3. Add a `wifi0` alias so m1n1 can inject the XR MAC, antenna SKU and
   calibration from the live ADT/syscfg.
4. Use board/module identity `yodak`, subject to the filename expected by
   the Linux brcmfmac PCIe device ID; never hard-code Bad Apple's `geniek`.
5. Enable `brcmfmac`, `brcmutil`, PCIe WLAN support and useful debug output.
6. Extract and rename the matching iOS BCM4357 firmware/NVRAM/CLM files into
   `/lib/firmware/brcm/`.
7. Keep PCIe ASPM disabled initially and verify association and traffic before
   attempting power saving.
8. Add `iw`, `wpa_supplicant`, and a DHCP client to the RAM userspace.

Success criterion: `wlan0` associates with an access point, obtains an IP,
passes sustained traffic, and continues working after USB is unplugged.

## Remaining work: Bluetooth (N88 over UART1)

### Why UART on XR, but PCIe on XS/11?

This is a board/module distinction, not an A12-versus-A13 rule. The local
N841 Apple ADT has a `bluetooth,n88` child **under `/arm-io/uart1`**, with
`transport-speed=3000000`, `transport-encoding=3`, BT wake/power functions,
and BTRx/BTTx/BCAL calibration. The XR's Linux PCI enumeration shows only
`14e4:440a` function `01:00.0` (Wi-Fi, class `0x028000`); there is no
Bluetooth `01:00.1` in the boot log. The XS Max reference DT instead models
BCM4377 Wi-Fi `14e4:4488` function 0 and Bluetooth `14e4:5fa0` function 1.
This shows the **host-facing paths differ**; it does not prove the radio is
on a separate physical chip. A combo module can expose Wi-Fi over PCIe and
Bluetooth over UART.

Independent checks: iFixit's own teardown identifies different module
markings (XR `339S00580`, XS `339S00551`); TechInsights identifies the XS
`339S00551` package as BCM4377. SEEMOO/InternalBlue lists XR with UART
devices and XS/11 with PCIe, but explicitly marks XR/XS untested, so it is
supporting evidence rather than proof of the exact XR protocol. Its firmware
list associates XR with BCM4347B1 Bluetooth; that identification still needs
confirmation from a successful XR HCI version response or iOS logs. Sources:
https://www.ifixit.com/Teardown/iPhone+XR+Teardown/114123 ;
https://www.ifixit.com/Teardown/iPhone+XS+and+XS+Max+Teardown/113021 ;
https://www.techinsights.com/products/far-1810-802 ;
https://github.com/seemoo-lab/internalblue/blob/master/doc/ios.md ;
https://github.com/seemoo-lab/internalblue/blob/master/doc/firmware.md .
The current HCI Reset timeout points to initialization/transport details,
not to a reason to reclassify XR Bluetooth as PCIe.

### Comparable Linux Bluetooth/UART ports on GitHub

- **iPhone 7 Plus D111 (closest Apple reference):** Pauli1Go's HoolockLinux
  firmware/bring-up notes report BCM4355C0 Bluetooth via kernel serdev,
  automatic `brcm/BCM.apple,d111.hcd` Patchram loading, and validated
  discovery, connection and A2DP audio. Its Wi-Fi is PCIe, so this is direct
  precedent for split Wi-Fi/BT host transports on an iPhone. Source:
  https://github.com/Pauli1Go/HoolockLinux-linux-firmware/blob/main/iphone7.md
- **Samsung Galaxy S20 Ultra Exynos (closest split-bus Broadcom example):**
  BCM4375 Wi-Fi uses PCIe while Bluetooth uses UART1 and `hci_bcm`. The
  project says `hci0` comes up; it needed a Samsung USI UART RX clock-request
  fix. The README does not establish a scan/pair test, so do not overstate it.
  Source: https://github.com/Bentlybro/z3s-mainline-linux
- **Raspberry Pi 4 (useful failure analogue, not a phone):** a mainline
  `hci_bcm` bring-up had the same `0xfc18` and `0x0c03` timeouts while Wi-Fi
  worked. Its issue traced this to **BT_ON/RTS ordering**: the controller
  expected BT_ON, a wait, then RTS, whereas the driver had asserted RTS
  first. This is a hypothesis to test for N841, not proof it shares that
  requirement. Source: https://github.com/raspberrypi/linux/issues/3260

The iPhone 7 result shows that Apple's UART transport can work under Linux;
the Samsung case shows that PCIe Wi-Fi plus UART Bluetooth is ordinary for a
combo module. For XR, prioritize Apple-equivalent power/wake/RTS timing and
UART RX clock/pin setup before trying PCIe Bluetooth or firmware loading.

**Status 2026-11-02: not working.** Three controlled XR Linux boots reached
`hci0` on UART1, but no HCI command completed. The original N841 ADT gives
UART1 `0x235204000`, IRQ 387, `bluetooth,n88`, `transport-speed=3000000`,
SMC `gP09` power and AP GPIO 6 BT_WAKE. The DTS attaches generic Broadcom
H4 serdev, supplies `current-speed=max-speed=3000000`, and injects syscfg
`BMac`. This establishes a *Linux HCI object*, not a communicating chip.

| Boot log | Result |
|---|---|
| `xr-bluetooth-firstboot.log` | With the UART at 115200, two `Frame reassembly failed (-84)` messages, then HCI Reset `0x0c03` timeout. |
| `xr-bluetooth-3mbaud.log` | Host configured for 3 Mbps but Apple S5L UART divisor still 16x (effectively 1.5 Mbps): `0xfc18` baud-command timeout. |
| `xr-bluetooth-oversample.log` | Apple S5L oversampling patch corrected UBRDIV to `0x00080000` (8x at 24 MHz, hence 3 Mbps); `0xfc18` still times out, then Reset times out. No controller version or firmware name obtained. |
| `xr-bluetooth-no-maxspeed.log` | Removed only `max-speed` from the DT and repacked the same kernel. The `0xfc18` command disappeared as intended, but the first HCI Reset `0x0c03` still timed out. Linux reached `/init`; the 3 Mbps host-rate change alone is not the fix. |
| `xr-bluetooth-smc-power.log` | Changed only the Bluetooth power path to direct ADT-style `pKW4(gP09, enabled)` via SMC, rebuilt the kernel, kept 3 Mbps with no `max-speed`. Linux reached `/init`, USB configured, and the first HCI Reset `0x0c03` still timed out. No firmware identity was learned. |
| `xr-bluetooth-uart-diagnostics.log` | Bounded logging only, same power/baud/protocol settings. The 4-byte H4 Reset packet (`type=0x01`, opcode `0x0c03`) was fully accepted by the Linux serdev write (`accepted=4`, `cmd_tx=1`, `byte_tx=4`). By the timeout `evt_rx=0`, `byte_rx=0`, `err_rx=0`: the host received no bytes at all. This proves software submission to the UART driver, **not** electrical transmission on the wire. Linux reached `/init`, USB configured; no kernel panic. |

`hci_bcm` defines `oper_speed` as a *different speed to program into the
controller*, or zero if the controller already runs at `init_speed`. Our
`max-speed=3000000` wrongly requests an early vendor baud-change command
`0xfc18` despite `current-speed` already being 3 Mbps. Removing `max-speed`
while retaining `current-speed=3000000` made the first command a normal HCI
Reset. **Tested: this removed `0xfc18` but did not make the controller
respond**. The no-max-speed image is preserved at
`/tmp/xr-bluetooth-before-smc-power.img4` (SHA256
`33704edff1c38c23f00f936138208f3b15a5cd396e16bec7f905022bfec4d633`).

Power is another unresolved difference. The N841 ADT's `function-power_enable`
is an SMC `pKW4` call with key `gP09` and argument zero. The previous Linux
`shutdown-gpios=<&smc_gpio 9 0>` uses `gpio-macsmc`, which writes
`CMD_OUTPUT | value`; Cascadia's iPhone 11 PCIe power fix instead uses the
ADT function directly (`ARG | 1`). A GPIO reported `out hi` does not prove
the SMC performed the same power operation. The matching XR iOS
`AppleBluetooth` KEXT looks up `function-power_enable`; its power-on/off
call sites pass a pointer to 1/0 respectively to the platform function.
This matches the ADT's `pKW4(gP09, 0 | enabled)` rather than the generic
GPIO encoding. A new Linux candidate adds optional
`apple,power-function` to `hci_bcm`, uses `apple_smc_write_u32` with the
ADT-style word, and removes `shutdown-gpios` from N841. It built and packaged
without error and **was boot-tested; HCI Reset still timed out**. SHA256 of `devices/a12/boot/inputs/m1n1-linux.img4` is
`08964774b988cc6c731087c745476c510e7e61a5f6a072cf394305f2fd4dd566`.
The 0020 diagnostic patch limits TX/RX messages to eight each and logs the
HCI counters only at timeout. It does not read MMIO or change transport
behavior. Its packaged image SHA256 is
`c55834c6fc6f82421eba32ec5546f918429cbf354394be02eb3f82fc0cfd9b6c`;
the pre-diagnostic image is at `/tmp/xr-bluetooth-before-uart-diagnostics.img4`.
The 0020 boot's first HCI Reset (`0x0c03`) queued and accepted all four TX
bytes, but the timeout counters were `cmd_tx=1 byte_tx=4 evt_rx=0 byte_rx=0
err_rx=0`. Acceptance by serdev does not prove transmission on the wire.
`serdev-ttyport.c` sets `CRTSCTS` on open unconditionally, and Samsung UART
termios enables hardware AFC for that flag. N841's ADT instead describes
`function-rts` as AP GPIO 32. **Untested hypothesis:** AFC/RTS handling is
blocking this UART exchange. Patch 0021 adds the N841-only DT property
`apple,no-hw-flow-control`, making `hci_bcm` clear `CRTSCTS` after serdev
opens, before HCI setup. It leaves power, baud, protocol, and the diagnostic
patch unchanged. The incremental kernel and DTB built successfully; the
packaged candidate `devices/a12/boot/inputs/m1n1-linux.img4` has SHA256
`4dcbf0fbcbcccc2e4789456e89eb1c5fe018cc1b933190ab0958a2c0ed26f37a`.
The previous image is `/tmp/xr-bluetooth-before-no-flow.img4`. **Boot-tested
2026-11-02:** serial capture `XRa1n/logs/serial/xr-bluetooth-no-flow.log`
confirms `hardware flow control disabled`, then HCI Reset `0x0c03` accepted
all four TX bytes but timed out with `cmd_tx=1 byte_tx=4 evt_rx=0 byte_rx=0
err_rx=0`. Linux reached `/init` and USB configured; no kernel panic was
seen. Clearing `CRTSCTS` alone therefore did **not** restore communication.
UART pinmux, wake/reset sequencing, controller speed, and protocol remain
open questions. Do not infer physical TX from serdev acceptance.
Next, check RTS/CTS pinmux, initial protocol framing, reset timing,
BT_WAKE polarity, and whether N88 actually speaks generic Broadcom H4. Avoid
live `/dev/mem` probing: a UART register diagnostic coincided with the phone
returning to recovery. Work from saved logs and firmware until a deliberate
new boot is ready.

**GPIO diagnostic correction (2026-11-02):** Two m1n1 read-only GPIO probe
boots failed before Linux. The first read used ADT's bus-relative
`0x3c100000` and faulted with an L2 translation fault (ESR `0x96000006`).
Mapping that same address produced a synchronous external abort
(ESR `0x96000018`, FAR `0x3c100074`). The Linux DTS proves the AP GPIO
physical address is `0x23c100000`: the ADT `reg` is relative to `/arm-io`
and the initial probe omitted the `0x200000000` bus translation. The failed
m1n1 probe is disabled as `devices/a12/m1n1/0010-*.patch.disabled`; do not revive it
or retry either failed image. The current boot payload uses the byte-identical
known-good m1n1 SHA256 `11efec7c...d8b4` extracted from the pre-probe IMG4.

**Boot-tested:** `linux-0022-xr-bt-gpio-snapshot.patch` moves
the six read-only GPIO register reads to the Linux pinctrl probe, after the
platform device's power domain is attached and its physical resource is
ioremapped. N841 alone opts in via `apple,bt-gpio-snapshot` in its DTS. The
kernel and DTB built; the new IMG4 SHA256 is
`84b5515e24d33e688b4e41e444b9432e2c8c5c6019bd8859ca910babe25ffc82`.
The 2026-11-02 boot reached Linux `/init` and the USB composite gadget.
Serial log: `XRa1n/logs/serial/xr-bt-gpio-linux-probe.log`. At pinctrl probe
the retained AP GPIO values were: pin29 `0007418e` (periph0/input0),
pin30 `00072a21` (periph1/input1), pin31 `000743a1` (periph1/input1),
pin32 `00072602` (periph0/input1), pin33 `00074220` (periph1/input1),
pin34 `00072a21` (periph1/input1). The ADT names TX30 and RTS32; the
adjacent pins are comparison data, **not confirmed** RX/CTS assignments.
Pin32 was retained in GPIO mode rather than peripheral mode. HCI Reset
still timed out (`cmd_tx=1 byte_tx=4 evt_rx=0 byte_rx=0 err_rx=0`).

**Next diagnostic prepared, not yet phone-tested:** the pristine N841 ADT was
decoded directly. `/arm-io/uart1` is `0x235204000`, IRQ 387; `function-tx`
names AP GPIO 30 and `function-rts` names AP GPIO 32.  Its `bluetooth,n88`
child specifies 3,000,000 baud, encoding 3, BT_WAKE GPIO 6, and
`pKW4(gP09, ...)` power.  The ADT does not name RX or CTS, so adjacent pins
31/33 remain observations rather than asserted wiring.  Patch
`linux-0023-xr-bt-uart-register-diagnostics.patch` adds bounded, read-only
Samsung UART register logging after real FIFO writes and in the Apple UART
IRQ handler.  It does not change pinmux, baud, power, flow control, or HCI
framing.  Built kernel SHA256 is
`8f7283f93cdaa8b63da3fff613da813ce2d87a4cdb5d20d551d821261755ec99`;
the packaged SysCfg-injected IMG4 SHA256 is
`4509f68c4e01130c2138089c6be33391312a87b3a6b880534d7d4a965a2fea31`.
The previous boot-tested image is preserved at
`/tmp/m1n1-linux-pre-uartreg.img4` (SHA256 `84b5515e...ffc82`).

**Boot-tested successfully (2026-11-02):** the working iPhone 7
Bluetooth DT series showed that the board must explicitly select all four
UART pins and use hardware RTS/CTS.  N841 now selects AP GPIOs 30, 31, 32 and
33 as peripheral function 1, removes `apple,no-hw-flow-control`, opens N88 at
the standard Broadcom identification rate of 115200, and requests the ADT's
3,000,000 operational rate through `max-speed`.  XR-specific SMC power,
BT_WAKE GPIO 6, BMac injection and the bounded UART register diagnostics are
unchanged.  DTB SHA256 is `42165d7b...29745`; packaged IMG4 SHA256 is
`1a1bcba593f618f5bd0e31a3520fa6ebd9bb1abe0c37b9dd117ea03b8543931c`.
The immediately previous 3-Mbit/no-flow diagnostic image is preserved as
`/tmp/m1n1-linux-uartreg-3m-noflow.img4`.
The phone answered the initial Broadcom `0xfc18` baud command at 115200,
transitioned to 3 Mbit/s, answered HCI Reset and identification commands, and
reported chip ID 137, `BCM4347B1` / `BCM4349B1 (002.002.014) build 0000`.
The initial firmware blocker was that btbcm requested, in order,
`BCM4349B1.apple,n841.hcd`, `BCM4349B1.hcd`, `BCM.apple,n841.hcd`, and
`BCM.hcd`, none of which was present.  Serial log:
`XRa1n/logs/serial/xr-bt-pinctrl-115200-3m.log` (SHA256
`863fee61...1a6c`).

**Firmware loaded successfully from RAM (2026-11-02):** `BlueTool` was
extracted from the matching XR iOS 18.7.10 (22H374) OS image.  Sandcastle's
`hcdpack` selected `BCM4357B1 Olympic Yoda-K_US USI`, matching SysCfg's
`wifi_module_vendor=USI`.  The resulting image is stored outside the
initramfs as `devices/a12/boot/inputs/brcm/BCM4349B1.apple,n841.hcd` (SHA256
`9cbde294...e365`).  After Linux boot, it was copied over USB SSH into
`/lib/firmware/brcm/` and `serial0-0` was rebound through
`/sys/bus/serial/drivers/hci_uart_bcm`.  The controller accepted the patch
and reported `BCM4357B1 Olympic Yoda-K_US USI` and
`BCM4349B1 (002.002.014) build 1072`; `hci0` remained registered.  Capture:
`XRa1n/logs/serial/xr-bt-hcd-live-load.log`.  The next blocker is supplying a
small Bluetooth management userspace (`btmgmt`/BlueZ) to bring the interface
up and verify scan, pairing and traffic.

BlueZ 5.87 management userspace (`btmgmt`, `hciconfig`, `bluetoothctl`,
`bluetoothd`) plus D-Bus and all ARM64 runtime libraries is provided as the
external RAM bundle `devices/a12/boot/inputs/bluez-root.tar.gz`.  Embedding it grew
the initramfs enough for m1n1 gzip decompression to raise an SError, so both
BlueZ and the HCD deliberately remain external.  The loader extracts the
bundle into the RAM root; `/sbin/start-bluetooth` initializes the system bus,
starts `bluetoothd`, and powers index 0.
`devices/a12/boot/load-bluetooth-firmware.sh`
copies it into RAM, rebinds `serial0-0`, powers on index 0 and prints controller
state.  `devices/a12/boot/boot-xr-linux.sh` preserves the exact DFU upload sequence and
starts a fresh timestamped DCSD capture; `devices/a12/boot/boot-and-load-xr.sh` then
waits for USB SSH and loads both Wi-Fi and Bluetooth firmware automatically.

The first discovery test reached the patched controller but failed with
`Opcode 0x2041 failed: -56`. Linux maps this to HCI status `0x01` (Unknown
HCI Command): firmware build 1072 advertises LE extended scanning but rejects
LE Set Extended Scan Parameters. Patch 0024 adds a DT-selectable
`brcm,broken-ext-scan` property to `hci_bcm`; N841 enables it so Linux uses
its existing `HCI_QUIRK_BROKEN_EXT_SCAN` legacy-scan fallback. Rebuilt
candidate IMG4 SHA256 is
`605970e34193d84d33250d1cac812ae41829e71a1e96875647c42b5176e3490f`.
The rebuilt kernel was boot-tested with external BlueZ/HCD loading. A timed
`bluetoothctl scan on` started discovery without opcode 0x2041 errors and
found both BR/EDR and LE devices, including a nearby TV with service UUIDs and
manufacturer data. This verifies UART transport, patchram, BlueZ power-up and
radio scanning. Pairing and sustained profile traffic remain to be tested.

## Alpine/XFCE over USB NFS (candidate ready, 2026-11-02)

The XR now follows Project Cascadia's iPhone 11 split-root design rather than
putting a desktop into `rkrn`: the small recovery initramfs brings up ACM/ECM,
then mounts an Alpine root exported by the host over NFSv3 and calls
`switch_root`. This keeps the raw rkrn payload at 13,069,754 bytes and avoids the m1n1
decompression SError previously caused by embedding a large BlueZ bundle.

`devices/a12/rootfs/build-alpine-rootfs.sh` builds Alpine 3.24.3 ARM64 with XFCE,
Xorg/fbdev, libinput, Dropbear, Wi-Fi and native Alpine BlueZ. The current
tarball is `devices/a12/build/alpine-xr.tar.gz` (SHA256
`8e870a27fc0658fdf69a4c35924da92d5150cc4a0f099748cb4c4a0f00c446e9`).
`devices/a12/host/linux-nfs-export.sh` installs it at
`/srv/cascadia-xr-root`, exports it only to `172.16.42.0/24`, and supports
both systemd hosts and this non-systemd Linux host. A local NFSv3 mount test
verified `/sbin/init`, XFCE and Bluetooth tools through the live export.

The kernel was rebuilt with NFSv3/NFSv4 client support; `Image.gz` SHA256 is
`4667bc3401a871797f5b400ce6fd7ac83798e092c467ca828a3efb2413d9f7c7`.
The initramfs contains `etc/nfsroot` set to
`172.16.42.2:/srv/cascadia-xr-root`, retains the recovery shell if mounting
fails, and preserves ECM through a chrooted USB watchdog after `switch_root`.
The packaged SysCfg-injected test image is
`devices/a12/boot/inputs/m1n1-linux.img4` (SHA256
`43de82819065261c8ede292bb61a07603958c182742098fae6d7f76f78d8509e`).

Bluetooth loading detects Alpine and uses its native musl BlueZ instead of
extracting the external Debian/glibc bundle. The HCD and Wi-Fi firmware remain
small files loaded over USB SSH after boot. First phone test checklist:

1. boot with `devices/a12/boot/boot-and-load-xr.sh` and keep the host NFS services up;
2. verify serial reports `[nfs] mounted ...` and `switch_root`;
3. verify SSH reconnects at `172.16.42.1` and `/etc/alpine-release` exists;
4. inspect `/var/log/xfce.log`, `/dev/fb0`, touch events and the visible XFCE
   session;
5. then load Wi-Fi/Bluetooth firmware and repeat radio smoke tests.

**Do not copy the iPhone 11 Bluetooth driver.** N104 uses BCM4378 PCIe
function 1 and `hci_bcm4377` with IPSW firmware and calibration. Its project
notes verify that `hci0` comes up but do *not* document an XR-compatible UART
transport or a successful Bluetooth pairing/scanning test. The XR's N88 is a
separate UART controller. Its `BTRx`, `BTTx`, `BCAL` data are not yet consumed.
After a successful HCI Reset, identify the chip, then resolve firmware and
calibration before testing scanning and pairing.

Success criterion: `hci0` scans and pairs with a known device and transfers
data without destabilizing Wi-Fi.

## Execution order

Keep each milestone independently bootable and preserve USB SSH throughout:

1. **Board data audit:** extract N841 touch, panel and BCM4357/N88 properties from
   the live/pristine Apple ADT and matching IPSW.
2. **Touch first:** smallest self-contained hardware path; keep display on
   simpledrm.
3. **Wi-Fi:** PCIe port 2 BCM4357 plus `yodak` firmware/calibration.
4. **Bluetooth:** independent UART1/N88 path.
5. **Native display takeover/backlight:** highest risk of losing the visible
   console, so do it after SSH and wireless recovery paths are reliable.
6. **UI:** a RAM userspace with libinput/Wayland or Xorg; GPU acceleration is
   not required for the first usable desktop.

## Safety and regression checklist

For every new payload:

- retain simpledrm until native display takeover is proven;
- retain ACM shell, ECM, Dropbear and `usbwatch`;
- use DFU/recovery volatile uploads only;
- do not mount or write Apple NVMe namespaces;
- preserve a known-good `m1n1-linux.img4` and its SHA256;
- test boot, `/dev/fb0`, USB reconnect, ping and SSH before testing the new
  driver;
- collect DCSD serial only when needed and clean large logs afterward.

### J171 TCLK and post-upload validation audit — 2026-11-05

Status: static analysis complete for the clock operation and power ordering;
not implemented or hardware-tested in this audit. The iPad is in normal iPadOS.
Do not connect to Linux SSH or reboot it without a new test request.

Evidence is from the exact 21B91 kernel in IDA and J171a firmware/ADT, with
saved decompilations in `/tmp/ipad8-research-audit/`.

**Confirmed missing clock control**

- J171 ADT `function-clock_enable` targets PMGR `TCLK` with arguments
  `[8, 100, 24000000]`. These are enable delay, disable delay and requested
  frequency; 8 is NOT a divider.
- `ApplePMGRFunctionEnableTouchClock` initializer `0xfffffff008e9401c`
  stores the delays and computes divider `24000000 / requested_hz`, hence 1.
  Its call handler is `0xfffffff008e93f64`.
- T8020 register-group initializer `0xfffffff0090e5f28` maps touch group 1
  to PMGR register bank 0, offset `0x78000`. With this board's ADT mapping,
  that is PA `0x23b078000`, inside our existing PMGR syscon resource.
- Native enable `0xfffffff008e7c020`: read old value; clear bit 31; write;
  delay 8 us; preserve unrelated bits, replace bits 9:0 with divider 1 and
  set bit 19; poll until bit 18 clears. Native requires divider 1..1023.
- Native disable: clear bit 19; poll bit 18 clear; set bit 31; delay 100 us.
- Delay helper `0xfffffff0082912b8` scales by 1000 (microseconds), whereas
  sequence helper `0xfffffff008291298` scales by 1000000 (milliseconds).
- Linux currently supplies the touchscreen `clocks = <&clkref>`, a fixed
  24 MHz clock. `clk_prepare_enable()` on this provider does not perform
  the native PMGR writes. This is a confirmed port gap, not proof that the
  bootloader left the physical clock disabled on the last test.

**Power/reset ordering**

The exact firmware Config/Device Tree Overrides power sequence is:
`power_ana -> power_ldo -> display_sync -> display_sync1 -> clock_enable`.
Clock sequence record is `0f 01 03 00`; reset record is `01 01 01 00`.
Native function-sequence execution (`0xfffffff008b30224`, step
`0xfffffff008b2ed3c`) executes forward on enable and reverse on disable,
with the recorded delays after each successful operation. The clock step
has a 15 ms forward settle. The present J171 power-on code already places
clock enable after SMC rails and sync GPIOs and before this settle/reset.

Port as a real clock provider using PMGR syscon/regmap; select it only in
J171 DT. Keep common/XR clock consumers unchanged. Use bounded busy polling,
readback diagnostics and error propagation. Fix the J171 power-off early
return so a newly acquired clock reference is released on probe failure or
unbind; do not introduce unverified SMC rail shutdowns. Re-enabling/retries
must not be skipped because a stale `clk_enabled` boolean survived failure.
The dedicated touch reference is separate from SPI controller input `nclk`;
implementing this clock does not validate the separate 200 MHz SPI assumption.

**Post-upload IRQ validators / no blind extra wait**

- Native boot entry `0xfffffff008b5ffa0` marks upload pending. Initial reset
  IRQ enters `interruptAction` at `0xfffffff008b613f0`, checks HBPP readiness,
  and calls `loadFirmware` (`0xfffffff008b32790`). Only after upload does it
  mark booted and arm Normal Boot Ms (500 from this firmware configuration).
  A later IRQ selects SPI profile 2 and activates the runtime protocol.
  Thus native evidence does not establish TWO POST-UPLOAD IRQs on J171.
- Our existing `device-booted` completion log is a host label, not a decoded
  device message. Consuming one IRQ must not by itself certify Z2 readiness.
- Arm a distinct firmware-release completion/IRQ epoch BEFORE the final CM4
  reset-release RMW; discard earlier reset/HBPP notifications without losing
  an IRQ arriving during the final transfer/ACK. Keep counters/timestamps and
  GPIO level as diagnostics, not as substitutes for protocol validation.
- Require the final image-valid and reset-release RMW ACKs to be `4ad1`.
  Currently DATA ACKs are strictly checked (`4bc1`) but RMW ACKs are logged
  without an equivalent strict rejection gate. Match the 21B91 RMW validator.
- Enforce the configured 500 ms post-release timeout. Do not accept a timeout
  or replace a missing edge with an unconditional state advance. Do not add
  a second J171 post-upload wait solely to imitate J172.
- After the notification, validate runtime DeviceInfo: command E2, complete
  16-byte response, checksum, and usable advertised packet-size bounds before
  runtime reads. Preserve the existing later runtime-frame/report checks.
- Log full EE/E2/E1 TX/RX and actual SPI CFG/CLKDIV at the boundary. Repeated
  `4f81` is NOT a validated DeviceInfo reply; its precise firmware/ROM meaning
  remains unproven. Do not bypass validation or label touch operational.

Latest hardware result remains: corrected HBPP14 zero-based DATA word count
passes all DATA packets with first-attempt `4bc1`, final RMWs ACK `4ad1`, one
post-release edge observed, then DeviceInfo returns repeated `4f81` and probe
fails with -EPROTO. No further hardware result is implied by this audit.

#### Implementation/build follow-up — 2026-11-05

Implemented `linux/extras/clk-apple-t8020-touch.c`, adapted with original
HoolockLinux/Corellium attribution. It uses the PMGR syscon at the native
T8020 offset, divider limits 1..1023, prepare/unprepare callbacks (bounded
50 ms busy polling), and logs the inherited and programmed register values.
Only the J171 DT instantiates this provider; the touchscreen now references
it instead of a fixed-clock stub. Clean-build staging is in build-kernel.sh.
J171 error cleanup asserts reset and releases its clock reference, without
changing the SMC power-rail policy.

Post-release completion is armed before the final RMW, with an IRQ-count
epoch and a separate FIRMWARE_IRQ state. Only validated DeviceInfo advances
to DEVICE_INFO. RMWs now reject non-4ad1 ACKs. Startup EE/E2/E1 TX/RX and the
first 12 matching SPI configuration/divider transactions are logged. No
additional post-upload IRQ wait or forced-PIO boot argument was added.

Incremental `make ARCH=arm64 LLVM=1 -j8 Image.gz` succeeded; only changed
clock/touch/SPI objects and normal link/image generation were rebuilt.
The .config was unchanged. An extracted-C host test passed enable/disable
ordering, bit preservation, delay values, invalid-rate/read-error handling,
and busy-timeout rollback. DT validation confirms the touchscreen phandle
resolves to the new provider; the saved XR DTB hash is unchanged. The linked
vmlinux contains the provider's probe/prepare/unprepare functions.

Controlled test package: `/tmp/ipad8-j171-tclk/linux.img4`
SHA256: `e9471e27c2052260f4809d995ccc47a67098d6d56e5c50e2cf21dc01f49265c0`.
Uses the same m1n1/bootargs and corrected wordcount initramfs as the previous
boot-tested package, replacing only kernel and J171 DTB. IMG4 extraction
byte-matches the raw payload. Logs/config/hash manifest/tests are alongside
it. The generated shared initramfs was NOT rewritten: use this explicit
controlled test image, not an older normal bootset. No hardware boot,
commit or push was performed for this follow-up.

#### TCLK hardware boot result — 2026-11-05

Booted the explicit TCLK test image with a fresh serial capture after deleting
old iPad test serial logs. Current log `/tmp/ipad8-j171-tclk/dcsd.log`.
TCLK inherited `0x800002ee` (disable bit set); prepare programmed `0x80001`
(enable plus divider 1), busy clear. Firmware DATA/RMW upload passed.
Post-release IRQ epoch 1 -> 2 arrived after 8365 us. DeviceInfo succeeded
on attempt 1: `e2 28 11 e0 22 00 00 00 00 00 00 00 00 00 1d 02`.
Checksum is valid; advertised maximum packet size is 0x22e0 (8928 bytes).
This confirms the previous repeated-4f81 DeviceInfo failure is resolved on
this test. Next runtime read fails `packet too large: 4520` against the old
4000-byte RX buffer. Touch is NOT operational yet. Linux/normal Debian NFS
handoff succeeds. Serial capture remains running; no further reset performed.

Read-only USB SSH collection completed before the user's next reboot:
`/tmp/ipad8-j171-tclk/ssh/{dmesg.txt,state.txt}`. DeviceInfo checksum was
independently rechecked; maximum advertised packet is 8928 bytes. No touch
rebind, SPI command, device register write or process restart was performed.
Clock framework shows the touch clock reference count returned to zero after
probe failure, consistent with the new cleanup path. The 4520-byte frame
was rejected before a large runtime transfer; it was not read out of bounds.
Next change should size/bound runtime buffers from the validated advertised
maximum (with a hard cap and transfer-alignment overhead), not suppress the
length check or treat the already-registered input name as working touch.

### J171 receive-path reference audit — 2026-11-05

Analysis only: no buffer or packet code was changed/rebuilt in this audit.
Matched 21B91 AppleHIDTransportProtocolZ2 decompilation was checked against
its ARM64 instructions. Evidence is `/tmp/ipad8-research-audit/rx-*`.

1. `initializeTransactionBuffers` at `0xfffffff008b688e0` allocates a report
   descriptor using the DeviceInfo u16 maximum at object+0x13e. It also
   allocates a shared descriptor of max+2048 bytes, with the runtime transfer
   window starting at offset 2048 and capacity max. The 2048 is host-side
   buffer layout, NOT a wire header or an instruction to add 2048 clocks.
   Our device advertised 0x22e0 = 8928. The inherited Hoolock buffer is 4000.
2. `deviceGetResultLength` at `0xfffffff008b6c798` sends a checksummed
   16-byte EB/tag command. It REQUIRES reply[0]==E1 and verifies the complete
   16-byte reply checksum (`checkChecksum`, 0xfffffff008b66848) BEFORE
   reading LE16(reply+1). Checksum is sum(first14) modulo 65536, stored LE16
   at bytes14..15. Our receive path currently checks only E1 before length.
3. `readOneFrameOfData` at `0xfffffff008b6c478` treats a zero announced length
   as no pending data (no subsequent large SPI transfer). For nonzero L it
   passes L+5 to the result read. `deviceReadResultData` at
   `0xfffffff008b6caa0` checks this logical total against the advertised max,
   then rounds using the protocol's configured Round To Bytes. The exact
   K1F19,9 profile supplies 1. Our unconditional `(L+8)&~3` rounds to FOUR.
   Therefore 8928 is the native maximum full logical result-transfer size,
   including its five-byte frame header, not an 8928-byte payload allowance.
4. Result transfer is full duplex with A5 dummy TX; native sets the first
   16 dummy bytes, uses a shared transfer window, and checks marker EA/EB,
   the zero-modulo-256 sum of the first five frame bytes, then the LE16
   payload length at frame+2. For nonempty data, payload includes its final
   two checksum bytes: sum(payload without checksum) modulo65536 must equal
   the LE16 trailer; dispatch receives payload length minus2. Linux must
   retain bounds checks on this inner length against bytes actually read.
5. Native tag changes 1 <-> 2 after successful read processing. There is a
   five-attempt frame-read retry/resynchronization path; a failed attempt
   does not advance the tag. Do not change parity simply because a larger
   buffer now permits the transfer. Our existing strict payload tag check
   should be assessed using the first captured full result, not removed
   speculatively. Multiple report/idle frame handling remains to be tested.

Correction to the earlier runtime-size diagnosis: 4520 was the size our
legacy formula COMPUTED, and the EB/E1 length reply was not logged or
checksummed. We have not captured a valid 4520-byte payload/result yet.
For example, L=4512 produces legacy4520 but native4517 for this profile.
The fixed4000 cap is demonstrably incompatible with the advertised8928,
but the exact first frame size must be established from a validated header.

Next implementation scope: J171-only validated maximum/capacity after
DeviceInfo; checksum the length response before consuming L; no large read
for L=0; reject logical L+5 above the advertised max or allocation; use
Round To Bytes=1 for J171, retaining other boards' behavior; bound inner
payload and check its checksum/tag before dispatch. Update long control
report bounds using the same actual capacity. Keep command TX16 separate
from the resizable full-duplex result window; no arbitrary 16 KiB constant
or disabled length check. Log the full length reply and raw/logical/wire
lengths so the next test can distinguish framing from report parsing.
Independent native-rule boundary tests passed checksum corruption, zero,
exact max, oversized/u16 max and 1-vs-4 alignment cases; these are static
boundary tests, not evidence that an incoming frame has been read.

### J171 receive fixes implemented/rebuilt — 2026-11-05

Implemented the reference-audited receive changes in apple_z2_j171.c:

- Validate DeviceInfo first, then allocate a J171 runtime RX/duplex window
  to its u16 advertised maximum. Track real allocation capacity separately
  from protocol maximum; retain 16-byte command TX storage. Allocation
  failure keeps the old pointer intact. Other boards retain their defaults.
- Decode the length reply only after checking E1 and its complete 16-byte
  checksum. Nonzero L transfers exactly L+5 bytes on J171 (alignment1), and
  must fit both the advertised maximum and the actual allocation.
- A validated zero-length reply performs no bulk SPI transfer and reports
  no input. It is a valid idle transaction, with native tag advancement;
  probe may proceed on this explicit J171 idle condition, without falsely
  claiming a valid input frame. Other boards' first-frame gate is unchanged.
- Preserve header/payload checksums, inner-length bounds, and strict tag
  validation before dispatch. Errors do not advance parity. Use explicit
  8-bit J171 transfers for odd byte lengths. Long control-read bounds now
  reference the same advertised/allocation limits, not a fixed4000 limit.
- Log the first eight length replies and validated pending/logical/wire
  sizes; frame failures expose bounded headers, not unbounded log dumps.

Incremental eight-job kernel build succeeded with the unchanged .config.
Nineteen extracted-C tests passed under UBSan/bounds instrumentation, with
mocked SPI, output guards/canaries, max/odd/idle/bad-header/checksum/counter/
inner-length/transfer-error cases and non-J171 regression cases. Reproduce:
`python3 devices/a12/linux/tests/test-j171-rx.py`. This is framing/control-flow
validation, not a test of physical SPI or complete touch-report decoding.
ASan could not initialize on the host's limited user VA layout; that separate
host limitation is recorded in the test artifact directory, not hidden as a
successful ASan run.

New controlled package `/tmp/ipad8-j171-rx/linux.img4`:
SHA256 `2fb7401655af862671639cc50448d9092793400bbf0cb30e4a89fb661943f783`.
Only the kernel differs from the boot-tested TCLK package. m1n1, command
line, TCLK DTB and corrected firmware initramfs are byte-identical; IMG4
extraction matches the raw package. XR DTB hash and kernel config unchanged.
Build/test logs, before-source, inputs and manifest are in that directory.
No new boot, remote device changes, commit or push performed. Use this
explicit image for the next test rather than the older normal bootset.

#### Receive-fix hardware test — 2026-11-05

Booted explicit RX image with serial capture `/tmp/ipad8-j171-rx/dcsd.log`.
TCLK, HBPP upload, post-release IRQ, first-attempt DeviceInfo and runtime
allocation8928 all passed. The new validated length replies expose:

- First E1 header `e1 12 00 e2 00 00 e1 00 00 ee 00 00 00 00 a4 03`,
  checksum valid, L18, exact wire23. Result starts
  `ea 01 92 c0 43 20 41 00 72 00 43 00 50 04 00 00`; it fails frame
  validation (header sum not zero; inner length exceeds bytes read).
- A subsequent IRQ-side read gives validated E1 header
  `e1 a3 11 e1 00 00 eb 01 00 e2 00 00 00 00 44 04`, L4515, wire4520.
  Result starts `e1 12 80 eb 21 40 1b 61 20 1d c0 80 e1 00 00 00`,
  failing the EA/EB result marker. No frame dispatched, probe returns -71.

Thus the old fixed-size rejection is gone and 4520 is now a validated wire
length on the later transaction, but the full-result stream is not valid.
Do not describe touch as working. Next investigate command/result pipeline,
native retry/resynchronization and full-duplex SPI reception; do not simply
raise buffers again or suppress checksum/header validation. Linux reaches
/init; capture remains running. No additional reboot performed.

#### Native SPI receive boundary audit — 2026-11-05

The above capture was subsequently stopped at the user's request; do not
restart it or boot automatically. Kernel IDA session `f5a64760` on
192.168.1.17:8746 was reachable for this audit. Evidence is saved under
`/tmp/ipad8-research-audit/current-spi-*.json`.

Completed comparison through the transport, controller configuration,
PIO setup, DMA ordering and Z2 retry layer:

- AppleSamsungSPI version-1 constants at 0xfffffff007455180 decode to
  word-size shift15, CFG base0x4000, bit-order mask0x2000, FIFO depth16;
  status setup is0x40000f. Configuration builder code at
  0xfffffff009026340 uses the device profile, not transfer length. It
  imposes minimum word-delay2 for version1 when the profile requests
  the extra clock-polarity behavior. IDA merges this BTI entry into the
  preceding function; check its addressed instructions rather than
  trusting the merged pseudocode's apparent stack-check branch.
- Native common preparation at 0xfffffff009027ea4 writes divider,
  word-delay, status mask, base configuration and control for both
  PIO and DMA. Native PIO at 0xfffffff00902737c writes one byte per
  TX FIFO register entry, matching our 8-bit packing; there is no
  evidence here for reversing bytes or changing runtime to16 bits.
- Our full-duplex PIO selector enabled version1 setup only for <=16
  words. At23/4520 words it dropped the minimum word delay (2->0,
  absent a DT override) and0x40000f status setup. The CFG value alone
  can remain0x10401e across this boundary, so logging CFG alone misses
  this difference. This is a confirmed setup mismatch, not yet proof
  of the observed corrupt-frame root cause.
- Corrected the selector for J171 full-duplex PIO at every length.
  Other compatibles, RX-only and TX-only selection remain unchanged.
  This selects register setup; it does not allocate a hardware/DMA
  descriptor or assume the entire transfer fits the FIFO. Existing
  refill/drain logic still handles transfers larger than16 words.
  Added bounded first12 setup logs including length, word-delay and
  selection, including EB length requests and bulk result reads.
- Native transport0xfffffff008b62490 chooses DMA at the configured
  threshold (17 in this profile). Native DMA0xfffffff009026808 arms
  RX before TX, programs both counts, starts the controller and waits
  for both completion paths. Our full-duplex DMA is still missing;
  do not equate TX-only firmware-upload DMA with runtime RX support.
- Native readOneFrameOfData0xfffffff008b6c478 retries up to5 times.
  On a retry it sleeps1ms, clears its synchronized flag, and makes
  two checksummed length requests separated by the profile delay
  before reading data; parity advances only on success/valid idle.
  Our receive path currently lacks this resynchronization. Keep it
  separate from this first transport-setup test rather than hiding
  a corrupt frame by dispatching it or blindly advancing the tag.

Host tests: `python3 devices/a12/linux/tests/test-j171-spi-setup.py`
extracts the actual selection/delay functions and checks J171 lengths
1..65535 (including16,17,23,4517,4520,8928), non-J171/XR-compatible
selection, TX-only and RX-only behavior, CPOL and configured delays.
Passed with UBSan/bounds, as did all19 existing extracted-C receive
tests. These do not validate electrical timing or actual FIFO operation.
Eight-job incremental compilation completed successfully with unchanged
config; compiled SPI source matches the staged repository source. New
controlled package: `/tmp/ipad8-j171-spi-setup/linux.img4`, SHA256
`6667cc53f6b82901d134f5de3182e9a5dc7a18df6d20cf6f5c128cf5a9b9fe8c`.
Packaging verified that only the kernel changed relative to the tested
RX package: m1n1, arguments, DTB and corrected firmware initramfs remain
byte-identical. Extracted IMG4 payload matches the packaged raw payload.
No device boot, logger restart, commit or push performed. Do not claim
hardware success before a new captured test. On that test, verify the
16-byte and larger result transfers both log word-delay2 and
version1-setup1, then require valid EA/EB headers and payload checksums.

#### SPI setup correction hardware test — 2026-11-05

User requested capture and boot. Verified iPad11,6 / CPID8020 / BDID24
in USBLiter8-pwned DFU and both image hashes. Started capture first,
confirmed `/dev/ttyACM0` opened, sent audited serial iBSS, then DeviceTree,
SmartIO firmware, trust cache and the explicit SPI-setup Linux image.
All upload commands completed successfully. Capture:
`/tmp/ipad8-j171-spi-setup/dcsd.log` (left running as requested).

Linux reached /init and userspace. DeviceInfo and allocation8928 passed.
Both16-byte queries and larger reads now log word-delay2 and version1
setup1. Crucially, the first23-byte result no longer logs an invalid
frame and initialization advances into `apple_z2_j172_init_reads_locked`
(21-byte and130-byte control reads observed). That path is gated by
successful initial runtime validation, so this is hardware evidence
that the first-frame corruption was corrected, not just a setup log.

Touch probe still fails later: an EB result-length request at tag2 gets
`e3 73 00 10 00 00 00 00 00 00 00 00 00 00 66 01`, rather than E1.
The reply has a valid16-byte checksum but the wrong message type;
driver rejects it with -EPROTO/-71. Do not relax the E1 check. Next
trace the report-control command/result pipeline and pending IRQ
interaction during initialization. This is a later failure than the
previous malformed first23-byte frame; touch is not working yet.

#### First-failure analysis of the E3 reply — 2026-11-05

Correction to the preliminary interpretation above: the E3 seen by the
IRQ reader is a secondary symptom, not evidence by itself that the EB
transaction caused the initialization failure.

`apple_z2_j172_init_reads_locked()` imposes `prop_len == 8` for report73.
Captured E3 bytes decode as report73, status0, length16, with checksum
0x0166 valid. Initialization reaches surface read21 (16+5), then DB
read130 (125+5), then report73 info. It rejects the advertised16-byte
report before issuing its data read. The function returns -EPROTO,
releases io_lock while `booted` is still true; a pending IRQ can enter
`apple_z2_read_packet()` before the outer failure cleanup clears booted.
That IRQ clocks out the still-pending E3 control reply and logs the
misleading result-length error. Its tag2 is consistent with the first
runtime frame already having passed and advanced parity.

Verified this causal sequence using the actual extracted report-info
and init-read functions with a mock supplying D9 length16, DB length125,
and73 length16. It returns -EPROTO after exactly two report-data reads,
never attempts report73 data, and leaves the byte-identical captured
E3 reply in RX. Replay passed UBSan/bounds:
`python3 /tmp/ipad8-j171-spi-setup/replay-report-init.py`.
This validates the software failure sequence; the init-stage E3 query
itself was not logged at info level on hardware, so the replay is not
an additional hardware trace.

Native comparison in IDA session f5a64760:
- deviceGetReportInfo0xfffffff008b69920 decodes status and LE16 length;
- getReportGated0xfffffff008b69ca4 uses the advertised length, requires
  <=512 and sufficient destination capacity, rather than a fixed8;
- deviceGetReport0xfffffff008b6a5a4 chooses short for <=11, long for >11,
  validates response type/checksum and copies the advertised data.
Evidence: `/tmp/ipad8-research-audit/report-cause-*.json`.

Next correction should be J171-specific advertised-length report73
handling with native bounds and E6/E7/report/status/checksum validation,
not globally changing8 to16 and not accepting E3 as a runtime frame.
Also prevent runtime IRQ consumption during failed/control init cleanup.
No driver modification, new build, reprobe or reboot in this analysis.

#### Report73 correction implementation — 2026-11-05

User authorized implementation, tests, incremental rebuild and packaging,
then readiness notification (not an automatic boot).

- J171 report73 uses its advertised nonzero length up to the native512
  limit; other boards retain the8-byte requirement. This is not a global
  replacement of8 with16. Actual report reads retain advertised packet
  and allocated-window bounds, and now enforce512 for J171.
- Added J171-only short/long response validation: exact transfer size,
  E6 for <=11 payload bytes / E7 otherwise, matching report ID, status0,
  and checksum. Long J171 reads explicitly request8-bit SPI words.
- J171 leaves booted=false until all init reads and writes succeed.
  During control initialization IRQs only record completion; they cannot
  enter the runtime reader. Runtime IRQ waiters recheck booted under
  io_lock before consuming data. Non-J171 startup behavior is unchanged.
- Tests: `python3 devices/a12/linux/tests/test-j171-reports.py` extracts
  actual report-info, init-read and response-validator C. Mocked lengths
  8/16/512 succeed for J171; zero/513 reject, non-J171 still rejects16,
  read errors propagate. Short/long boundary lengths1/8/11/12/16/125/512
  validate, wrong type/report/status/size/checksum reject. Passed under
  UBSan/bounds. Existing19 receive tests and SPI setup regression pass.
  Mocked tests do not prove hardware touch or IRQ scheduling correctness.

Eight-job incremental kernel build completed successfully, retaining the
existing config and SPI setup correction. Staged and repository touch
sources match. Controlled output: `/tmp/ipad8-j171-reports/linux.img4`,
SHA256 `d862718794934def4315b587098c22e9313ea31ca3f9400b17fee6e0de976c54`.
Verified only the kernel differs from the captured SPI-setup package;
m1n1, arguments, DTB and corrected firmware initramfs are byte-identical.
IMG4 extraction matches the raw package. Ready for a requested captured
test; no reboot, commit or push performed. Touch success remains unverified.

#### Report-fix hardware test — 2026-11-05

At user request stopped old capture PID20992 and deleted the old
SPI-setup/RX dcsd.log files. Fresh capture is
`/tmp/ipad8-j171-reports/dcsd.log`, left running. Verified pwned DFU,
image hashes, then completed the established RAM-only boot sequence.
Upload logs are in the same directory. Linux reached /init.

Hardware confirms report73 advertises16 and initialization now proceeds
through the later report reads and9D/BF/AF setup, rather than failing
at the8-byte check. No touch probe-failed line observed in this capture.
Runtime reception is still not healthy: first EB gets a checksummed
pending E3/AF status2 reply, then an EA frame appears during a length
query, followed by an invalid4132-byte result read and zero replies.
Do not equate successful initialization with working touch. Next inspect
control-to-runtime synchronization (native retry/double-length-query
path already traced), validation of write acknowledgments and the
extra polling consumer. No further build or reboot performed.

#### Bounded runtime resynchronization — 2026-11-05

User requested implementation and rebuild. Split the existing receive
operation into one-attempt framing plus a J171-only bounded wrapper:

- At most5 attempts per call; wait1ms before each retry.
- After an error, require two independently validated16-byte E1 length
  replies before any data read, using the existing inter-packet delay.
  If the first query fails, stop that attempt without issuing a bulk
  read; if the second fails, likewise retry without advancing parity.
- A successful frame or valid idle clears resync state and advances the
  tag once. Failed headers/checksums/size/transfer errors never dispatch
  input and never advance the tag. Exhaustion returns the last error
  and leaves resync requested for the next call. Unlike native iOS's
  kill-device path, this wrapper does not power-cycle/reset the device.
- Mark the control-to-runtime handoff as requiring double-query resync,
  even without a prior runtime error. Other compatibles retain their
  existing single-attempt behavior. All size/checksum/counter gates stay.

`test-j171-rx.py` retains19 one-attempt framing cases and adds8 wrapper
cases: stale E3, corrupt frame then recovery, resync idle,5 I/O errors,
5 wrong markers, bad second-query checksum, initial handoff and non-J171
no-retry regression. All passed UBSan/bounds, as did report and SPI setup
suites. An initial test-harness insertion error was corrected before
these successful runs; it did not require a driver change.

Eight-job incremental build completed successfully in the existing build
tree, with unchanged config and staged source matching the repo. Package:
`/tmp/ipad8-j171-resync/linux.img4`, SHA256
`82344854a2210926f76a90931ec77706c1b07c1d2345b7fe7478c6bf1ca8e52d`.
Verified only the kernel changed from the captured report-fix image;
m1n1, command line, DTB and corrected firmware initramfs are unchanged.
IMG4 extraction matches raw package. Tests/build logs are saved alongside
the image. No new boot, device reset, commit or push performed. Hardware
resynchronization and touch success remain unverified.

#### Retry image hardware test — 2026-11-05

User requested old log deletion, fresh capture and boot. Stopped logger
PID23605, deleted `/tmp/ipad8-j171-reports/dcsd.log`, and started fresh
capture `/tmp/ipad8-j171-resync/dcsd.log` before serial iBSS. Verified
pwned iPad11,6 DFU and both hashes; completed all RAM-only uploads and
bootx successfully. Linux reached /init and USB gadget configured.

Retries execute: first runtime reply is pending E3/AF, next EA while
querying length; subsequent E1 replies validate and tags later alternate.
However, around17.98 seconds replies become all zero and the bounded
five-attempt loop repeatedly exhausts with -71. This test does not
establish working touch; retry alone did not resolve stable reception.
Do not infer a specific hardware crash from zero replies alone. Capture
left running; no additional reboot or patch performed in this boot turn.

#### Static firmware/runtime audit — 2026-11-06

No driver changes, build or boot in this audit. Firmware IDA uses raw
Kona image at 0x100000. Its initialized RAM is compressed in the file:
scatter loader0x130108 first copies0x290c bytes140d50->14e8d8, then
decoder130158 expands0xdb88 bytes to140d50, then zeros BSS14e8d8.
Treating raw file bytes as RAM constants caused incorrect Hex-Rays
constant folding. Selected volatile annotations improved decompilation;
no firmware instruction/data bytes were patched.

Offline replay of the three scatter entries establishes startup bytes
1412d2=2 and1412d3=2. Normal76 producer1183a2 checks bit1 of1412d2;
scheduler118970 copies1412d3 to1412d2. ReportAC setter117d20 changes
1412d3. Therefore a missing AC enable is not a proven startup blocker.
BF shadow1412f0 starts9d310900, independently matching the captured E3/BF
response. Reconstructed pointer14e8b0 points to144ee0, which is algorithm
root144d38+424: BF apply118002..11800c updates those algorithm flags.
The target itself is zero immediately after scatter loading; algorithm
initializer124e7c subsequently installs319d. Our9d810900 write changes
bits12/13/15; their consequences are not yet established.

Firmware10f47c/10f59a and113f36 identify captured5a010200201000 as raw
sensor/debug data, not a fatal dump. Its4509-byte report plus protocol
overhead explains4520-byte wire frames. Normal12a678 emits76 with32-byte
header and48-byte float contacts;11920c routes it with mask3, so Grape
mask2 is correct. Native8b6cf10 walks multiple90 TLVs; our single-TLV
dispatcher rejects valid bundled reports in offline actual-C replay.
This is a confirmed parser gap, not proof of this capture's root cause.

Native AC references8b58f84 etc belong to HSMDeviceTest.h/image-mode test
code, not evidence of the selected production boot recipe. A5's PMGR
mapping correction is useful methodology but its dead-SPI failure is
earlier than our valid DATA ACKs/DeviceInfo/runtime responses. Preserve
these distinctions rather than copy another board's register formulas.

Evidence/scripts/IDA exports are retained under
`/tmp/ipad8-research-audit/`; latest consolidated analysis is
`scatter-and-report-findings.md`, initialization replay is
`reconstruct-scatter.py`. Hardware authority remains
`/tmp/ipad8-j171-resync/dcsd.log`: pending E3/AF and EA at runtime handoff,
then valid E1 lengths, then zeros from17.9878s. No complete touch fix or
controller crash has been established. Next causal questions remain BF
effects, native IRQ/wake lifecycle, and normal76 delivery.

BF consumer follow-up: the BF apply destination is the algorithm flags at
root144d38+424, not AC report mode. The319d->819d low-halfword change is
XOR b000 (bits12/13/15). A local candidate scan of1400 named firmware
functions, checked against Thumb/decompilation, identified processing
consumers at12310e/12312a (bit0),125ce8 (bit9),12a95a (bit1) and12d1ba
(bit2); these bits are unchanged by our BF write.12a644 copies the full
word into auxiliary report metadata. No traced processing branch tests
the changed bits, so BF is not an established explanation for missing
touch or zero SPI replies. Do not invent names for those high bits or
claim this local scan rules out every indirect consumer. Full trace:
`/tmp/ipad8-research-audit/bf-consumer-findings.md`. No driver edit/build.

#### J171 port corrections and package — 2026-11-06

Applied at user request, without changing XR matching/driver paths:

- J171 sync pins57/58 now select peripheral2 via a named pinctrl state,
  rather than GPIO output pulses. Native chain: function-sequence type3
  -> GPIO function89da2f8 -> GPIO mode3 in89d7644, mask270/value240.
  Linux pinctrl programs peripheral2/input enable; actual electrical sync
  activity is not established by the state-selection success message.
- Initial J171 DeviceInfo wake uses2000us, not125ms. Native config
  8b6d564 sets Wake Delay Us=2000;82912b8 scales to microseconds.
- J171 dispatcher walks all bounded90 TLVs, including empty records,
  dispatching each Grape76. Other compatibles retain the single-TLV path.
- J171 BF is read with E6 and written back unchanged, rather than replaced
  with J172's9d810900. Read failure aborts initialization.
- J171 short-write E1 replies validate marker, checksum, status and last
  command/report. This deliberately fails mismatches instead of silently
  accepting them. No new retries or speculative power resets added.
- Initramfs builder regenerates an existing J171 firmware container before
  copying it. Its old build/touch artifact was stale. Fresh converter output
  matches the boot-tested corrected firmware hash99d9efea...ad864b.

Focused diagnostics: successful sync-state selection, BF preservation,
rejected write status, first4 runtime IRQs, first8 received frame TLV traces
and first4 Grape headers. Existing persistent logs retained. Poller/IRQ
lifecycle remains unchanged; no claim of a new IRQ recovery implementation.

Five extracted-C host test suites pass UBSan/bounds: dispatch, report/write
validation,19 receive cases plus8 retry cases, SPI setup, and BF initialization
flow. Legacy behavior is tested where shared helpers changed. These are
software tests, not hardware validation. Targeted diff checks and bash syntax
pass; an existing unrelated SPI patch has a pre-existing whitespace warning.

Eight-job incremental build compiled apple_z2_j171.o and relinked, using an
unchanged .config. Staged source cmp passed. DTB compiles, and decompilation
shows pins20039/2003a plus display-sync-active. New image:
`/tmp/ipad8-j171-portfix/linux.img4`, SHA256
`fb679279170e4cbcc6486a0d537764b43d51f3beeba93b22a08d9975504d78d4`.
IMG4 extract matches raw payload. The boot-tested initramfs is retained
byte-for-byte and its embedded J171 firmware matches fresh conversion;
m1n1 and bootargs unchanged, only kernel/DTB differ from the retry image.
Build, tests, conversion and package manifest/logs are alongside the image.

User subsequently supplied pwned iPad11,6 DFU (8020/BDID24). Fresh capture
`/tmp/ipad8-j171-portfix/dcsd.log` started before the verified serial iBSS.
Old logger stopped without deleting its log. DeviceTree -> SmartIO -> trust
cache -> new image -> bootx all sent successfully. m1n1 identified J171
and began decompressing the kernel. Runtime outcome still under observation.

Captured hardware outcome: Linux boots; sync pinctrl selection and native BF
preservation succeed. Three normal Grape 0x76 reports (68 bytes) are logged
around18.075s, alongside mask8/raw5A frames; this does not establish contacts.
Subsequently E1 result-length replies become all zero and retries fail.
User-reported brief cursor movement remains unconfirmed. A20-second direct
/dev/input/event1 capture recorded zero bytes of input events, and touch IRQ
count remained28->28. Live debugfs: GPIO208 input high; reset27 output high
(active-low descriptor); CS51 high; pins57/58 owned byspi0.0 inperiph2.
Clock framework reports touch-ref24MHz enabled and SPI200MHz enabled; these
are software/framework states, not electrical measurements. SPI statistics
show continuing synchronous transfers with zero controller errors/timeouts,
while protocol replies fail. IRQ thread sleeps inirq_thread, poller inmsleep.
Therefore neither desktop mapping nor controller timeout explains this stall;
firmware responsiveness/wake/handshake and actual signal behavior remain to
be determined. No reset, unbind, or new hardware register writes performed.
Evidence saved in /tmp/ipad8-j171-portfix/{touch-events.txt,live-state.txt,
live-irq-spi.txt,dcsd.log}. No claim of complete touch fix.

Runtime lifecycle follow-up (kernel IDA reconnected e581137b): native
interruptAction8b613f0 preserves deferred events; readOneFrame8b6c478
requests provider power-state2 (assembly w1=2,w2=back-to-sleep delay), then
five bounded reads and fatal recovery8b60d0c on exhaustion. Linux lacks
that deferred-event/recovery state machine and instead polls forever.
These are port gaps, not a demonstrated cause of the first zero response.
Important correction: native runtime frame reading does not directly call
EE wake either; verified wake calls are DeviceInfo/report control paths.
Last loggedGrape18.076489 -> firstzero18.084747 (8.258ms), so no established
100ms idle-sleep explanation. J171 already takes the A5 full-duplex branch
because j172 flag includesj171; legacyspi_read is NOT its runtime path.
Detailed evidence /tmp/ipad8-research-audit/runtime-lifecycle-findings.md.
Firmware IDA8745 worker became unreachable; kernel8746 is available.
No speculative code changes/rebuild made; first-failure cause still open.

Kona firmware runtime follow-up (IDA64fad59b): actual Z2 parser11486a,
E1 reply builder1146d8, tag/current-entry state1411f8/1411f4, CS-edge
handler1136cc and publish gating114778 traced. Firmware normally returns
E1 status even for empty pending data/checksum error; zero replies are not
normal no-contact frames. CS-end113720 restores/arms status FIFO; publish
requiresnotbusy and14115a==1. Reset handler10d000->ROM hook->AIRCR reset
exists, but no hardware crash code proves that it ran. Full Grape status
beyond logged16bytes and actual CS/ATN/SPI register transition remain
missing. Deferred IRQ replay + IRQ-qualified service is the next controlled
comparison, not a confirmed fullfix. Analysis-only volatile annotations
prevent compressed-RAM constant-folding errors; firmware bytes unchanged.
Detailed evidence:/tmp/ipad8-research-audit/firmware-runtime-stall-findings.md.
No new source edit/build/boot performed for this analysis.

### J171 IRQ-owned runtime service and first-failure diagnostics — 2026-11-06

Implemented J171-only event service in apple_z2_j171.c. The former8ms SPI
polling loop is removed. Threaded IRQ records event tokens and wakes a
service kthread; pre-runtime edges at/after firmware-ready are latched and
replayed after initialization. An already asserted ATN at handoff also
requests one service. Tokens arriving during service remain queued;32-event
budget yields without dropping the remainder. Existing five-attempt/tag
resync stays unchanged. Runtime/PM lifecycle mutex precedes io_lock;
threaded J171 IRQ never waits for io_lock or performs runtime SPI.

After exhausted retries: snapshot first, release input contacts, disable IRQ,
assert reset and stop clock/sync, then permit only ONE reset+firmware-reupload
attempt. A subsequent failure parks service rather than rebooting forever.
This is not a verified full rail power-cycle, nor proof that recovery fixes
the initial failure. Suspend/remove quiesce service; resume starts a new
bounded session. Non-J171 IRQ service and XR HID driver path are unchanged.

Diagnostics deliberately distinguish observations from interpretation:
- Eight-entry runtime transport history includes boottime us, query/data,
  requested length, tag, SPI return code, TX16/RX32. Nonzero RX is not claimed
  valid until protocol checks; history SPI return0 is not protocol success.
- First all-zero length response dumps history and ATN/CS/reset raw levels,
  IRQ/pending counts, protocol state and software clock-enable state BEFORE
  driver recovery. GPIO208 is now described as interrupt-gpios in J171 DTB.
- SPI master captures CTRL/CFG/PIN/STATUS/FIFO/TXCNT/RXCNT/CLKDIV/WORDDELAY
  before finish_xfer clears active registers; prior nonzero-RX transfer is
  cached for comparison, explicitly not labeled protocol-validated. Max two
  zero episodes per master probe; consecutive zeros don't spend both slots.
  No RXDATA register is read by diagnostics, so FIFO data is not consumed.
  The TX opcode is saved BEFORE transfer because data TX/RX buffers alias.
- Full32-byte Grape headers (first4 per initialization) now expose previously
  omitted contact-count/status bytes. Failed transfers may leave stale RX;
  the transport error field must be considered when reading history.
- After5s without service, one QUIET message reports IRQ/ATN/state only.
  Silence is NOT called failure; this timer sends NO SPI probe or reset.
- SPI register time is before cleanup; history time is after spi_sync returns,
  possibly after serial diagnostic output. Neither is an oscilloscope capture.

Eight extracted-C host suites pass UBSan/bounds, including actual worker/IRQ
code event injection, deferred handoff, budget preservation, quiesce, recovery
ordering/limit and legacy IRQ; actual ring bounds/wrap/guards; SPI snapshot
latching, aliased TX/RX opcode retention and non-J171 guard. These are mocked
software checks, not physical touch or SMP stress validation. Baseline test
coverage for report validation, TLVs, receive/resync and SPI setup retained.
Build/test/evidence artifacts: /tmp/ipad8-j171-irq/. No flashing/boot or push
performed as part of these edits. Incremental Image.gz build uses existing
.config and eight jobs; package/hashes recorded below after final verification.

Verified incremental build/package: `/tmp/ipad8-j171-irq/linux.img4`, SHA256
`0533e6a8e23e9e4308a20b5400f409c87078c2cdae8dede8d78f0f7e68251df8`. Config/source cmp and packaged diagnostic
strings verified; IMG4 extraction matches raw payload. Original boot-tested
initramfs is byte-identical and embedded J171 firmware matches conversion.
New DTB only adds the ATN GPIO descriptor relative to portfix. Ready for
RAM-only captured test; not yet hardware-booted, no claim of touch fix.

### Hardware milestone: J171 touch confirmed on IRQ-owned runtime image

User requested old capture removal; stopped prior DCSD logger, deleted its
/tmp/ipad8-j171-portfix/dcsd.log, started fresh capture at
/tmp/ipad8-j171-irq/dcsd.log. Verified pwned iPad11,6 /8020/BDID24, exact
serial iBSS hashc440bd7a...27bb4 and new image0533e6a8...51df8. RAM-only
iBSS -> DeviceTree -> SmartIO -> trustcache -> Linux -> bootx succeeded.
Linux USB SSH and Debian systemd are up.

Initial stale E3/AF and EA length replies recovered through existing resync.
Three initial zero-contact68-byteGrape reports appeared around18.335s.
At60.309948s,116-byteGrape report has contact count1 atoffset22.
User reported touch worked. Verified directly through/dev/input/event1:
25-second capture311856bytes=12994input_event records,2604SYN_REPORTs;
2591ABS_MT_POSITION_X changes(range2256..16195),2588Y changes(5375..19089),
16tracking-ID changes including-1(contact release). IRQ totals462->3100
(sum across CPUs). This is physical-input evidence, not cursor inference.
Captured events saved at/tmp/ipad8-j171-irq/touch-events.bin and text
summarytouch-verification.txt. No FIRST FAILURE/zero-before-cleanup/
retry-exhaustion/bounded-recovery markers observed at verification time;
QUIET5s was followed by real contact events, so was correctly not a failure.

This establishes working touch on this tested image and session, not long-run
stability, multitouch accuracy or a uniquely proven electrical root cause.
The IRQ/deferred-service change succeeds without triggering recovery here.
DCSD capture remains active; no new reset/flash/commit/push performed.

Cleanup at user request: removed older/tmp tests, research exports and
reference clones. Latest/tmp/ipad8-j171-irq retained; required serialiBSS,
Apple DeviceTree,SmartIO,trustcache and structural ticket copied/hash-verified
into itsboot/ subdirectory BEFORE deleting old locations. Future boot must
use these bundled paths, not removed/tmp/ipad8-fw or rtkit-v2 directories.
Spiro MCP Node scripts plusnode_modules retained at/tmp/mcp-client-spiro.
Active system/agent IPC preserved. Cleanup reporttmp-cleanup.json; disk now
~19GiB free. Serial capture was stopped earlier; success log remains.
# J171a radio preparation — 2026-11-06

**Newest result (Bluetooth quirk test, supersedes the status below):** patch
0026 plus two J171-only DT opt-ins were built and boot-tested. Debian NFS
works; external MagnoliaA Murata build 1334 now exposes a management index,
powers on, uses the verified private BMac and discovers four nearby devices
(LE and BR/EDR). No pairing/profile test. Unknown advertising types 0x23/24
remain to investigate. An extra Wi-Fi scan after discovery hung and its
specific `iw` process was stopped; concurrent radio behavior is not fully
validated. Touch initializes without SIO timeout; prior no-trace boot was
user-confirmed. Latest package/logs: `build/ipad8-radio/bt-quirks/`, image
SHA256 `2995cfdeb193770361771ab423b0d7314a43b11c1a755009f95cda6034eb7b70`.
Details in `boot/IPAD8_RADIO.md`. No commit/push.

**Latest captured test:** removed SIO's synchronous SPI-DMA trace calls only,
rebuilt incrementally (one job / one linker thread / 2 GiB process limit),
and kept radio DTB, protocol/timeouts, initramfs and boot inputs unchanged.
The user confirmed working touch; no previous SIO configuration timeout.
Host NFS services were restored, and Debian systemd runs on the existing
NFS root. Murata Wi-Fi successfully scanned 12 BSS entries; connection and
traffic remain untested. Bluetooth patch build 1334 is accepted but opcode
0x204b still aborts initialization: btmgmt sees zero indices and power-on
fails Invalid Index. Bluetooth is NOT operational yet. Details and hashes:
`boot/IPAD8_RADIO.md`, logs `build/ipad8-radio/no-sio-trace/`. Below is the
earlier preparation state, superseded by this captured result.

See `boot/IPAD8_RADIO.md` for the board-specific workflow and limits.
The repaired 21B91 OS DMG matches ZIP CRC32 and Apple BuildManifest SHA-384.
Selective read-only APFS-FUSE extraction (256 MiB address-space limit,
approximately 5 MiB observed RSS) produced BCM4355C1 magnoliaa Wi-Fi files
and both MagnoliaA Murata/USI Bluetooth HCD candidates. The image is now
unmounted. Never feed this multi-chunk UDIF DMG to the standalone `lzfse`
CLI: it buffers the whole input and is not a DMG extractor.

J171a DTS now enables APCIE port 2 with SMC gP13 (confirmed in both native
`/amfm` and `uart2/wlan`), BCM4355 endpoint, UART1 Bluetooth power gP11 and
BT_WAKE AP39. UART1 RX31/CTS33/mux1 are inherited T8020 assumptions, not yet
verified on iPad hardware. XR DTS, loaders and shared driver source were
not changed by this radio preparation. Existing kernel config already has
the necessary drivers; no kernel rebuild performed.

The retained private SysCfg is A2270: WMac/BMac/WCAL are present but WSKU is
absent. `calibrate-ipad8-radio.py` produces a separate private J171a DTB
without inventing WSKU or exposing identifiers. `load-ipad8-radio.sh`
checks target board identity, offers a firmware-free OTP probe, and requires
an explicit verified vendor before choosing NVRAM/HCD. Device bus IDs are
discovered instead of assuming XR's names. Firmware remains outside the
initramfs. Four host DT/calibration/loader-guard tests pass. No J171a radio
boot, vendor identification, HCD acceptance, connection or traffic test has
been performed yet. Do not describe the radios as working until tested.

## 2026-11-07: default A13-style watchdog handoff

A12 initramfs now defaults to A13 handoff for both profiles. DHCP alone starts
before NFS discovery; cleanup precedes moving /proc; one static BusyBox worker
is started from /run/cascadia before switch_root. Debian replacement is skipped
by a RAM marker. Fallback starts an initramfs worker only if handoff fails.
XR and J171 source-built payloads passed automatic USB/SSH/NFS reconnect without
manual cleanup. J171 passed repeats/fresh boot; an earlier timeout remains in
the evidence. No kernel USB workaround was applied. XR kernel/m1n1 unchanged.
The prior separate RAM-root test was diagnostic and is not the default.
Six lifecycle host checks include default selection; separation tests retained.
Rootfs profiles/Quickstart/NFS guide document the default and quiet worker checks.
No commit or push performed.
