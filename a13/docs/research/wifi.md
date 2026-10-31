# Wi-Fi and Bluetooth on the iPhone 11 (BCM4378 on PCIe)

Status 2026-11-01: **works**. The phone joins a network, gets an address,
the internet goes over Wi-Fi, and the Mac reaches it by ssh with no cable.
Bluetooth's `hci0` comes up (nothing on top of it yet).

## The pieces

| | |
|---|---|
| Module | Broadcom BCM4378 B1 (`14e4:4425` Wi-Fi, `14e4:5f69` Bluetooth, one chip, two PCIe functions), "moana", vendor GODF (u / m) |
| Bus | `apcie,t8030`, port 2 (`pci-bridge2`), Gen 2 x1; DART `dart-apcie2` at 0x603008000 (inside the port's register block) |
| Power | the ADT's `/amfm function-reg_on` = SMC GPIO key `gP11`, written `ARG | 1` = 0x00000001 |
| PERST / CLKREQ | GPIO 35 / 33 (`function-perst`, `function-clkreq` on `pci-bridge2`) |
| Firmware | the iOS root filesystem: `System/Library/DriverExtensions/com.apple.DriverKit-AppleBCMWLAN.dext/Firmware` (Wi-Fi), `usr/share/firmware/bluetooth` -- `a13/cascadia periphery` fetches and renames them |
| Calibration | the phone's syscfg (NVMe namespace 3): WMac, WSKU (antenna "R"), WCAL, BMac, BCAL -- `pack` writes them into the DT |

## 1. The controller: m1n1 (patch 0005)

m1n1's `pcie.c` knew `apcie,t8103` and later, not `apcie,t8030`
("pcie: Unsupported compatible"). The IPSW's DeviceTree has no apcie
tunables, which made it look as if the whole sequence had to come out of
iOS -- but the **live** ADT iBoot hands over has them all (m1n1 exposes it as
`/dev/mtd1`, its own log as `/dev/mtd0`): `apcie-common-tunables`,
`apcie-axi2af-tunables`, `apcie-phy-tunables`, `apcie-phy-ip-tunables` on
`apcie`, `apcie-config-tunables` and `pcie-rc-tunables` on each bridge.

The register layout is the t8103 one (6 shared `reg` entries, 4 per port).
What differs, from `AppleT803xPCIe::_enableRootComplex` and
`::_initializePhy` in the 20E247 kernelcache (symbols present):

- the PHY IP tunables are one property, not `pll` / `auspma`;
- two PHY IP fields come from the PMGR fuses: fuse 0x5c bits 5:2 -> PHY IP
  0x400 bits 14:11, fuse 0x28 bits 16:12 -> 0xa3c bits 6:2
  (`_readFuseReg` subtracts 0xbc000: the offsets are inside `reg[5]`);
- no `rc+0x4` write; PHYIF run (`rc+0x24` bit 0) is followed by a 200 wait
  (IODelay or IOSleep -- m1n1 takes 200 ms); after the PHY `rc+0x34` bit 0 is
  cleared;
- the port registers are the t8103 ones (0x800 APPCLK, 0x804 STATUS, 0x810
  REFCLK, 0x814 PERST: `_setAppclkAutoDis`, `_setRefclkEnable`,
  `_setRootPortPerst`), so the generic port loop fits;
- only bridges 2 and 3 exist (3 is the baseband): a missing bridge must still
  step the ECAM base past its slot, and the Gen 2 ports have no
  `pcie-rc-gen3/gen4-shadow-tunables`.

Linux's `pcie-apple` then does the port itself (refclk, PERST, link
training); it already takes the port's PHY from `reg-names = "phy2"`.

## 2. The link: the module needs power (kernel patch 0006)

With the controller up the root port enumerated, the DART probed (no more
SError), and the link did not come up: `LINKSTS` 0xab000008, LTSSM started,
refclk on, PERST released -- nobody on the other end. A full iBoot leaves the
module powered; this boot chain does not. `/amfm` in the ADT has
`function-reg_on = <&smc 'pKW4' 'gP11' 0>`: writing `gP11` = 1 through the SMC
brought the link up at once.

Patch 0006 gives a `pcie-apple` port an optional
`apple,power-function = <&smc KEY ARG>` (the DT's port02 has
`<&smc 0x67503131 0>`): the driver writes `ARG | 1` with PERST asserted,
waits 50 ms, and goes on. Note: `ARG | 1`, the way iOS writes these keys --
not the `macsmc-gpio` encoding (`CMD_OUTPUT | value`).

## 3. Firmware after the switch to the root

The touch firmware loads in the initramfs; Wi-Fi and Bluetooth load theirs
later, with the NFS (or RAM) root in place, whose `/lib/firmware` is empty.
The initramfs now copies its `/lib/firmware` to `/run/cascadia/firmware` and
points `firmware_class.path` there. And `CONFIG_FW_LOADER_USER_HELPER` is
off: with the userspace fallback each missing name (brcmfmac tries several
board-specific ones first, `regulatory.db`) stalled its driver for 60 s,
nothing here ever answering it.

## 4. L1 ASPM

With L1 ASPM on (the endpoint's default) brcmfmac loads its firmware, sends
its first commands, and ~15 s later stops getting answers
(`brcmf_msgbuf_query_dcmd: Timeout`), interrupts still arriving. With ASPM
off from the start it works. `pcie_aspm.policy=performance` in
`boot/bootargs.txt` for now; the cause is probably CLKREQ / refclk gating
(`pci-l1pm-control` in the ADT), open.

## 5. Using it: `wifi`

`a13/initramfs/wifi`, the A5's command adapted: `wifi`, `scan`,
`connect SSID [PASS]`, `up`, `down`, `forget`, `auto on|off`, `reset`.
Networks in `/etc/wifi/networks/`, one file each, SSID in hex, a WPA
password only as its PSK. A13 differences:

- `wifi reset` restarts the chip by taking function 0 off the bus and
  rescanning (brcmfmac resets it and loads the firmware anew; Bluetooth stays);
- wlan0 comes ~15 s after the chip is found, so `wifi` waits up to 45 s;
- the cable's default route moves to metric 1000 (udhcpc's script gives
  wlan0 300 + its index): the internet goes over Wi-Fi, the cable stays the
  second way out;
- `p2p_disabled=1`: brcmfmac refuses wpa_supplicant's P2P device (-52) and it
  would retry every half minute.

The image carries `iw` and `wpa_supplicant` (and `wpa_cli`,
`wpa_passphrase`): `periphery` fetches Alpine's with their libraries and
musl loader into `/opt/wifi`, and small wrappers run them through
`ld-musl --library-path`, so they work in a RAM boot and under any root --
Wi-Fi is how a phone without the USB network gets online. (musl's `ldd`
reads only its first argument: one `ldd` per program.) The initramfs puts
`wifi` into the root on every boot and runs `wifi up` there when
`/etc/wifi/auto` exists.

## Found on the way

- The USB watchdog started a new `udhcpd` every 2 s (~400 after a while):
  started as `busybox udhcpd`, it is invisible to `pidof udhcpd`. It keeps
  its own `udhcpd -f` child now; `a13-init` no longer starts a second one.
- Testing `wifi down` over ssh with an inline `sh -c '... udhcpc -i wlan0'`
  kills the ssh session itself: `pkill -f` matches its command line. Run
  such tests from a script file.
