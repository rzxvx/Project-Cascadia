# J171: Wi-Fi and Bluetooth (BCM4355C1)

One Broadcom module, a Murata one on this iPad: Wi-Fi on PCIe (port 3 of
`apcie`), Bluetooth on UART3. Pauli1Go's drivers for the J172: the H9P
port's Wi-Fi lifecycle and its PCI power control (`PCI_PWRCTRL_APPLE_WIFI`,
`APPLE_T8010_WLAN`: the PMU GPIOs `reg_on` and `power_enable`), SysCfg as
NVMEM, brcmfmac taking its calibration from NVMEM, `hci_bcm` for the
BCM4355C1 on serdev. All built into the kernel here (the root has no
modules), `RFKILL` too (the panel reads it).

## Where the firmware is

Not in the IPSW's top level: in the **root filesystem image**,

- `System/Library/DriverExtensions/com.apple.DriverKit-AppleBCMWLAN.dext/Firmware/C-4355__s-C1/`:
  `rudderb.trx` (732185 bytes), `rudderb.clmb` (9097), `rudderb.txcb` (602),
  `P-rudderb_M-YSBU_V-m__m-2.5.txt` (5386), `P-rudderb_M-YSBU_V-u__m-4.3.txt`
  (5396) -- the firmware, its regulatory CLM, its TX power caps, NVRAM for
  two module vendors;
- `usr/sbin/BlueTool` (5221024 bytes), the Bluetooth Patchram images inside.

In iPadOS 18 the image is `094-32804-023.dmg.aea`: 5.9 GB (5905580032
bytes), stored in the zip uncompressed, **AEA-encrypted** (`com.apple.wkms`
in its header). Things tried before it:

- the iPad's own APFS from Linux: 7-Zip will not open a block device ("1
  file, 0 bytes"), apfs-fuse cannot open an iOS container ("Initialization
  of KeyManager failed": the keybag), and the kernel has no FUSE;
- over SSH from a jailbroken iPadOS: works, but needs the jailbreak boot.

With the image's AEA key (`sources.env`, public per build) macOS's `aea
decrypt` turns it into plain APFS -- `NXSB` at byte 32, no UDIF around it --
and 7-Zip (`-tapfs`, in an Alpine container) reads the two paths out. The
first 4 MB decrypted to 25 MB of APFS: a quick way to check a key. All
sizes and SHA-256 sums are Pauli1Go's (`ipad7.md`); `tools/firmware.sh`
checks every one.

## Wi-Fi

The files are named the six ways Pauli1Go's guide stages them --
`brcmfmac4355c1-pcie.apple,` + `rudderb`, `rudderb-XX`,
`rudderb-YSBU-m-2.5`, `rudderb-YSBU-m-2.5-XX`, `rudderb-YSBU-u-4.3`,
`rudderb-YSBU-u-4.3-XX` + `.bin`, `.txt`, `.clm_blob`, `.txcap_blob`: 24
files; the plain names fall back to the `m-2.5` NVRAM. With
`regulatory.db` (Alpine's wireless-regdb) they go into the initramfs: the
driver is built in and asks at boot.

The MAC and calibration: `pack` appends SysCfg to m1n1's payload, m1n1
reserves it (`syscfg@8ba4b8000`, 128 KiB) and publishes it as NVMEM; the
driver takes `WMac` and `WCAL`.

```
brcmfmac: brcmf_fw_alloc_request: using brcm/brcmfmac4355c1-pcie for chip BCM4355/12
brcmfmac: brcmf_c_process_txcap_blob: TxCap blob found, loading
brcmfmac: brcmf_c_process_cal_blob: Calibration blob provided by platform, loading
brcmfmac: ... Firmware: BCM4355/12 wl0: Jan 30 2025 ... version 9.30.516.0.3.50.104
```

`wlan0` has the iPad's own address. Scans see 2.4 and 5 GHz; joined to a 5
GHz network: 162/195 Mbit/s rx/tx at -72 dBm, DHCP, `apk` over it, ssh
over it. The regulatory domain stays world (`00`) until a country is set.
The ADT calls the J171's module "ruddera"; the "rudderb" files run it.

`wifi` (`initramfs/wifi`, the A13's with the iPad's PCI address 02:00.0 and
the cable's gateway 172.16.43.2): `scan`, `connect SSID [PASS]` (remembered
in `/etc/wifi/networks`, the password only as its PSK), `up`, `down`,
`forget`, `auto on|off`. DHCP's default route gets metric 300 + index; the
cable's stays at 1000, the fallback.

## Bluetooth

`brcm/BCM4355C1.hcd` (76184 bytes): Corellium's **hcdpack** (Project
Sandcastle, GPL-2.0; the one source file fetched at the commit Pauli1Go
pins, SHA-256 checked) cuts it out of `BlueTool` for chip `C-4355__s-C1`,
module `rudderB`, OTP NVRAM `M-YSBU_V-m__m-2.5`.

```
Bluetooth: hci0: BCM4355C1 (003.002.020) build 0000
Bluetooth: hci0: BCM4355C1 'brcm/BCM4355C1.hcd' Patch
Bluetooth: hci0: BCM4355C1 RudderB MUR MCC
Bluetooth: hci0: BCM4355C1 (003.002.020) build 1325
Bluetooth: hci0: BCM: Using default device address (43:55:c1:...)
```

"MUR": a Murata module -- the `m-2.5` image is the J171's as well. The
kernel logs the placeholder address at probe; by the time BlueZ asks,
the controller has the iPad's own (`BMac`, through NVMEM): `bluetoothctl
show` reports it as public. (A DT `local-bd-address` written by `pack` was
tried and dropped as unneeded.) BlueZ 5.86 powers it on and scans; pairing
not tried yet. RF calibration (SysCfg's `BTRx`, `BTTx`, `BCAL`) is not used
by the driver, as with Pauli1Go's.

`bluetooth` (the iPad mini 1's command): `on|off`, `scan`, `pair`,
`connect`, `disconnect`, `forget`, `auto on|off` -- on at boot unless
`/etc/bluetooth/auto-off` (the mini's default is the other way round).
