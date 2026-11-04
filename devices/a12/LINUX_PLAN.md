# Linux on iPhone XR (N841AP/T8020) — port status and plan

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
