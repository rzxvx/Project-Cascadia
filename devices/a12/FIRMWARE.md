# XR/N841 firmware inputs

All inputs are created locally; do not commit Apple payloads, the IM4M ticket,
or per-device SysCfg. The first-stage iBSS and pristine Apple DeviceTree are
from iPhone11,8 iOS **18.7.10 (22H374)**. `boot/make-inputs.sh` fetches only
those ZIP members from Apple's IPSW using `ipsw extract --remote`, unpacks them
with `img4`, and checks their exact SHA-256 hashes. No full IPSW download is
needed for these two files.

```sh
bash devices/a12/boot/make-inputs.sh
python3 devices/a12/boot/prepare-xr-linux-bootset.py \
  --ticket /path/to/this-XR/apticket.im4m --serial
# Or pass --no-serial for an iBSS without serial=3.
```

`prepare-xr-linux-bootset.py` also requires the locally built m1n1 Mach-O.
The first-stage Apple DeviceTree remains unmodified. The separate N841 Linux
DTB is built by `devices/a12/dts/build-dtb.sh` and included by `boot/package-linux.py`.

## Touch

`bash devices/a12/boot/make-touch-firmware.sh` range-fetches
`Firmware/N841_Multitouch.im4p` from the **same** IPSW and runs the existing
A13 conversion tool for XR personality `C1FC2,1`:

```sh
bash devices/a12/boot/make-touch-firmware.sh
```

The latter is the firmware's default calibration. On a real XR, use the
device's `MtCl` SysCfg calibration instead; it is private per-device data.
The rootfs/initramfs scripts consume these generated files, not the IM4P.

## Wi-Fi and Bluetooth

These are **inside the iOS OS filesystem**, not small top-level ZIP members.
For N841 on 22H374, the OS image is `094-31934-038.dmg.aea` (not the
`094-32062-038.dmg.aea` SystemOS cryptex). Obtain that IPSW member locally,
then run:

```sh
IPSW='https://updates.cdn-apple.com/2026WinterFCS/fullrestores/140-97953/5F3051CD-5405-4FEF-889E-C3B78E0F376F/iPhone11,8_18.7.10_22H374_Restore.ipsw'
ipsw extract --remote --pattern '^094-31934-038\.dmg\.aea$' \
  -o /path/to/local-cache "$IPSW"
bash devices/a12/boot/make-radio-firmware.sh /path/to/094-31934-038.dmg.aea
# An already-decrypted 094-31934-038.dmg also works.
```

The script asks `ipsw fw aea` to obtain the archive key, decrypts into a
temporary directory (~7.1 GiB), selectively extracts the DriverKit Wi-Fi
firmware tree and `/usr/sbin/BlueTool` with 7-Zip, and removes the temporary
DMG at exit. It converts Wi-Fi with `devices/a13/tools/brcmfw.py` (platform `yodak`)
and Bluetooth with pinned [Project Sandcastle `hcdpack`](https://github.com/corellium/projectsandcastle/tree/03db9c6ae04141eb940f3b9f56d446f50d57fadf/hcdpack),
selecting `BCM4357B1 Olympic Yoda-K_US USI`. The pin's source and the
resulting HCD are SHA-256 checked. The output HCD is named for the Linux
`btbcm` request, `BCM4349B1.apple,n841.hcd`.

Check that `devices/a12/boot/inputs/brcm/` contains
`brcmfmac4357b1-pcie.apple,yodak.bin`, the DRDO-u NVRAM text, and CLM blob
before calling `devices/a12/boot/load-wifi-firmware.sh`. Wi-Fi MAC, SKU and WCAL
come from this XR's private SysCfg; `XR_SYSCFG=/path/to/syscfg.bin` tells
`boot/package-linux.py` to inject them into the temporary DTB.

### This XR's private SysCfg

The Apple IPSW cannot supply per-device Wi-Fi/Bluetooth identity and
calibration. Once the XR reaches the **small Linux initramfs** with USB SSH,
read its 128 KiB NVMe namespace 3 without mounting or writing it:

```sh
bash devices/a12/host/read-syscfg.sh
# Writes devices/a12/boot/inputs/syscfg.bin (mode 0600, ignored by Git).
```

This script checks the namespace size and SysCfg magic, sets the block device
read-only, and validates that `WMac`, `WSKU`, `WCAL` and `BMac` are present.
It does not print their values. Do not commit, upload or publish the raw
SysCfg: it also contains device-specific identifiers. If USB SSH is not yet
up, first boot without `XR_SYSCFG`, capture SysCfg, and then repackage with
`XR_SYSCFG=devices/a12/boot/inputs/syscfg.bin` for calibrated Wi-Fi and the Bluetooth
address. The earlier `/tmp/xr-syscfg.bin` capture is not present locally now.

For XR Bluetooth over UART, the script extracts `BlueTool` from that same
22H374 OS image and writes
`devices/a12/boot/inputs/brcm/BCM4349B1.apple,n841.hcd`.
`devices/a12/boot/load-bluetooth-firmware.sh` transfers it after boot. The A13
`btfw.py` is for PCIe Bluetooth and is **not** the XR UART-HCD converter.

The recipe was reproduced against the retained 22H374 OS AEA: all 13 Wi-Fi
outputs and the HCD byte-matched the previously boot-tested files. Do not
substitute an arbitrary Broadcom HCD or commit generated firmware. The exact
per-device SysCfg and ticket are never published.
