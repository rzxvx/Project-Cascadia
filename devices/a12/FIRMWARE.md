# Firmware and per-phone data

Everything here is made locally from Apple's files and stays out of git: the
firmware, the IM4M ticket and the phone's SysCfg. All of it is from
iPhone11,8 iOS 18.7.10 (22H374).

## iBSS and device tree

`boot/make-inputs.sh` fetches only these two files from Apple's IPSW
(`ipsw extract --remote`), unpacks them with `img4` and checks their SHA-256.

```sh
bash devices/a12/boot/make-inputs.sh
python3 devices/a12/boot/prepare-xr-linux-bootset.py \
  --ticket /path/to/apticket.im4m     # --no-serial drops serial=3
```

`prepare-xr-linux-bootset.py` also needs the m1n1 Mach-O. The ADT goes to
iBoot unchanged, Linux gets `t8020-n841.dtb` from `dts/build-dtb.sh` inside
the payload.

## Touch

`boot/make-touch-firmware.sh` fetches `Firmware/N841_Multitouch.im4p` from
the same IPSW and converts personality `C1FC2,1` with the A13's `mtfw.py`:

```sh
bash devices/a12/boot/make-touch-firmware.sh
```

It writes `build/touch/n841-mtfw.bin` and the firmware's default calibration,
`n841-mtcal.bin`. The phone's own calibration is `MtCl` in its SysCfg.

## Wi-Fi and Bluetooth

These live in the OS image, `094-31934-038.dmg.aea` (not the
`094-32062-038.dmg.aea` cryptex):

```sh
IPSW='https://updates.cdn-apple.com/2026WinterFCS/fullrestores/140-97953/5F3051CD-5405-4FEF-889E-C3B78E0F376F/iPhone11,8_18.7.10_22H374_Restore.ipsw'
ipsw extract --remote --pattern '^094-31934-038\.dmg\.aea$' -o /path/to/cache "$IPSW"
bash devices/a12/boot/make-radio-firmware.sh /path/to/094-31934-038.dmg.aea
```

A decrypted `.dmg` works too. The script decrypts to a temporary directory
(about 7.1 GiB), pulls out the DriverKit Wi-Fi firmware and `BlueTool` with
7-Zip and deletes the rest. Wi-Fi goes through the A13's `brcmfw.py` for
board `yodak`. Bluetooth goes through Project Sandcastle's
[`hcdpack`](https://github.com/corellium/projectsandcastle/tree/03db9c6ae04141eb940f3b9f56d446f50d57fadf/hcdpack),
pinned and hash checked, which picks `BCM4357B1 Olympic Yoda-K_US USI`. The
HCD is named for btbcm, `BCM4349B1.apple,n841.hcd`.

The output is `boot/inputs/brcm/`: `brcmfmac4357b1-pcie.apple,yodak.bin`, the
DRDO-u NVRAM, the CLM blob and the HCD. They go over SSH after boot, with
`load-wifi-firmware.sh` and `load-bluetooth-firmware.sh`. The A13's
`btfw.py` is for PCIe Bluetooth, not this.

Rebuilt from the OS image, all 13 Wi-Fi files and the HCD match the ones the
phone was tested with.

## SysCfg

The IPSW has no per-phone data. Boot the small initramfs, then read NVMe
namespace 3 (128 KiB) over SSH:

```sh
bash devices/a12/host/read-syscfg.sh     # writes boot/inputs/syscfg.bin, mode 0600
```

The script checks the size and magic, sets the block device read-only and
checks that `WMac`, `WSKU`, `WCAL` and `BMac` are there, without printing
them. With `XR_SYSCFG=boot/inputs/syscfg.bin`, `package-linux.py` puts the
Wi-Fi MAC, antenna SKU, calibration and Bluetooth address into a temporary
copy of the DTB. SysCfg holds device identifiers: never commit or upload it.
