# Cascadia on Apple A10 — iPad 7

Linux on the 7th-generation iPad: **iPad7,11** (Wi-Fi, J171AP) and
**iPad7,12** (Cellular, J172AP), Apple A10 Fusion (T8010). Not the iPad 8
(J171aAP, A12): that one is in [devices/a12](../a12/README.md).

> **Status: bring-up, Alpine Linux in RAM.** On an iPad7,11 (J171), 2026-11-22:
> m1n1 (with Paul Praschl's T8010 NVMe patches) brings the NVMe link up and
> hands over to Linux 7.2; the kernel runs on both cores with 3 GB and
> cpufreq (Zephyr 300 MHz to Hurricane 2.24 GHz, stable under load), the
> screen (simpledrm + fbcon), the USB gadget (serial + network + SSH at
> 172.16.43.1), the internal NVMe (32 GB, all namespaces read-only), the
> battery and charging status, SmartIO, multi-touch (the Z2 firmware built
> with this iPad's SysCfg calibration), the on-screen keyboard (fbkeyboard,
> typing into the shell on the screen), Alpine 3.24 run from RAM with apk
> reaching the internet through the Mac. Not yet: Wi-Fi / Bluetooth
> firmware, the USB power source (`power_supply usb`: I2C NACK on J171). Photos:
> [gallery/](../../gallery/README.md#ipad-7-a10).

## How it boots

Tethered, from your computer, every boot. The chain is open source end to end:

| Step | What | Licence |
|---|---|---|
| checkm8 | [gaster](https://github.com/HoolockLinux/gaster) puts the iPad in pwned DFU and decrypts iBSS with its AES engine | Apache-2.0 |
| iBSS | iPadOS 16.4's, patched by [hBootPatcher](https://github.com/HoolockLinux/hBootPatcher) (`-airs`) | MIT |
| iBoot → m1n1 | iBSS loads the DeviceTree, an empty trust cache and m1n1 as the restore kernel (`bootx`), as on the A13 | MIT |
| m1n1 → Linux | m1n1 ([HoolockLinux/m1n1](https://github.com/HoolockLinux/m1n1), idevice) hands over to the kernel | MIT / GPL-2.0 |

This is the sequence of HoolockLinux's
[remote_boot](https://github.com/HoolockLinux/remote_boot), written here
as `./cascadia a10 inputs | pack | boot`. No checkra1n and no PongoOS.

## Sources

Pinned in [sources.env](sources.env):

- **Kernel**: [Pauli1Go/HoolockLinux](https://github.com/Pauli1Go/HoolockLinux)
  (Paul Praschl), the HoolockLinux fork with the J172 drivers: ANS NVMe,
  H9P PCIe Wi-Fi (BCM4355C1), SmartIO + SPI + Z2 touch, HDQ battery,
  SN2400 / D2365 charging, Bluetooth, ambient light. GPL-2.0.
- **Config**: HoolockLinux's `config_16k` + [kernel/a10.fragment](kernel/a10.fragment).
- **m1n1**: HoolockLinux/m1n1 `idevice`, the same commit as the A13 track,
  plus `m1n1/0001`–`0009`: Paul Praschl's T8010 commits from
  [Pauli1Go/m1n1](https://github.com/Pauli1Go/m1n1) (NVMe HMB, the NVMe
  link bring-up, SysCfg, touch and ALS calibration), which the kernel's
  drivers expect from the loader; `0010` adds the J171's HMB size.
- **m1n1** `0011`: the A13 track's patch that appends the iBoot ramdisk
  (the Alpine root) to the initramfs.
- **Kernel patches**: `kernel/patches/0001` fixes a NULL dereference in the
  H9P PCIe driver's probe error path.
- **Firmware**: `./cascadia a10 firmware` takes Apple's files from iPadOS
  18.7.9 (checked by SHA-256): the SmartIO firmware out of the kernelcache,
  and the touch controller's firmware (`J171_Multitouch.im4p`, the same
  image as the J172's), which Paul Praschl's makez2fw combines with this
  iPad's calibration from its SysCfg.

## Using it

```bash
./cascadia a10 doctor
./cascadia a10 build            # tools + m1n1 + kernel
# iPad in DFU, connected:
./cascadia a10 inputs           # once per board: iBSS + DeviceTree, decrypted and patched
./cascadia a10 firmware         # SmartIO firmware from Apple's CDN
# once, with Linux up: this iPad's SysCfg (read-only), then touch firmware
./cascadia a10 syscfg && ./cascadia a10 firmware
./cascadia a10 build fbkeyboard # the on-screen keyboard, into the next pack
./cascadia a10 build rootfs     # Alpine, run from RAM (sent as an iBoot ramdisk)
./cascadia a10 pack
./cascadia a10 boot             # Linux: ssh root@172.16.43.1, serial /dev/cu.usbmodemiPad7_linux1
./cascadia a10 boot proxy       # or m1n1's proxy, for m1n1's proxyclient
./cascadia a10 net on           # then: the Mac's internet for apk on the iPad (sudo, pf NAT)
```

`inputs` and `firmware` download only the few files they need from Apple's
CDN (partial IPSW reads). Nothing is written to the iPad's storage.
`build/private/syscfg.bin` and the touch firmware made from it carry this
iPad's serials and calibration: they stay in `build/`, out of git.

## Plan

1. ~~m1n1 proxy on the iPad 7~~ ✓
2. ~~Linux over USB~~ ✓ (shell over USB serial, network, SSH; the kernel
   log also on the DCSD UART, 115200)
3. ~~Alpine in RAM, internet over USB~~ ✓; next a Wayland desktop, labwc, as on the iPad mini 1
   (not XFCE on X11): touch, the on-screen keyboard, Wi-Fi, Bluetooth,
   battery. Rendered by pixman on the CPU until the GPU works.
4. What the kernel source does not have yet: speakers, auto-rotation
   (accelerometer), J171 (Wi-Fi model) checks where only J172 was tested.
5. The GPU (PowerVR-derived, Apple firmware on an RTKit coprocessor).
