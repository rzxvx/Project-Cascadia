# iPhone XR (N841AP, T8020)

Updated 2026-11-08. How each part was brought up, dead ends included, is in
[HISTORY.md](HISTORY.md).

## State

The phone boots tethered from DFU into Debian 13 with XFCE over NFS.
`boot-and-load-xr.sh` boots it and loads the radio firmware.

| Part | State |
|---|---|
| Framebuffer | works, iBoot's, through simpledrm |
| Touch | works, mapping not calibrated |
| USB | works: ACM shell, ECM, SSH |
| Wi-Fi | works, BCM4357 `yodak` |
| Bluetooth | works, scans; pairing not tested |
| Display driver | not enabled |
| GPU | not enabled, see below |
| Storage | read-only, never written |

## Hardware

Not the iPhone 11, and not the XS Max either.

| | iPhone 11 (N104, T8030) | iPhone XR (N841, T8020) |
|---|---|---|
| CPU | Lightning, Thunder | Vortex, Tempest |
| USB | DWC3, AUSB | DWC2, T8020 USB complex and DART |
| Wi-Fi | BCM4378, PCIe | `wlan-pcie,bcm4357`, module `yodak`, PCIe port 2 |
| Bluetooth | BCM4378, PCIe function 1 | `bluetooth,n88` on UART1, 3 Mbit/s |
| Display PMU | Chestnut | Chestnut on I2C2 at 0x27 (the XS Max has Acorn) |
| Touch | HID over SPI | HID over SPI, older `spi-1,samsung` block |
| Display | T8030 pipe, Pinot panel | H11P pipe, Pinot panel |

From the N841 ADT:

- Display: `disp0,t8020`, Synopsys `mipi-dsim-1`, panel `lcd,pinot`.
- Touch: SPI1, CS GPIO 67, reset GPIO 5, AOP interrupt GPIO 19, HBPP version
  `0x103`, image tag `wftm`, calibration `syscfg/MtCl`.
- Wi-Fi: PCIe port 2, calibration `syscfg/WCAL`, antenna SKU `syscfg/WSKU`.
  REG_ON is an SMC function, interrupt GPIO 22.
- Bluetooth: UART1, power through SMC `gP09`, wake GPIO 6, calibration
  `BTRx`, `BTTx`, `BCAL`.

## Boot

Upload order after usbliter8 starts the patched iBSS:

```text
iBSS.linux.serial.raw
DeviceTree.img4        the N841 ADT, unchanged
RestoreTrustCache.img4
m1n1-linux.img4        m1n1, Linux DTB, kernel, initramfs
```

The m1n1 patches in `m1n1/` add N841 to the platform list, T8020 USB clocks,
DWC2 DMA and PHY fixes, the USB DART, Vortex and Tempest chicken bits, CPU
rail voltages and PCIe from the live ADT tunables. `0006` only prints EP0
traffic on the UART.

## Kernel

A patch in `linux/patches/` does not mean its driver is on. The config has:

```text
CONFIG_DRM_SIMPLEDRM=y
# CONFIG_DRM_APPLE_H11P is not set
# CONFIG_DRM_AGX is not set
# CONFIG_TOUCHSCREEN_APPLE_MT_SPI is not set
# CONFIG_BT_HCIBCM4377 is not set
```

| Patches | What |
|---|---|
| 0001 to 0014 | badapple's T8020 base: AUSB, DWC2, AIC, SART, NVMe, temperature, H11P display, XS Max touch, legacy SPI, cpufreq, PCIe, BCM4377, DART |
| 0015, 0016 | BCM4357 in brcmfmac and its RAM base |
| 0017, 0018 | Bluetooth UART start rate, S5L UART oversampling |
| 0019 | Bluetooth power through an SMC function |
| 0020 | badapple's G11 GPU driver |
| 0024 | `brcm,broken-ext-scan`: firmware 1072 rejects LE extended scan |
| `extras/apple_hidspi_touch.c` | XR touch (HBPP), copied in by `build-kernel.sh` |

The XS Max's touch (0009) and BCM4377 Bluetooth (0013) are not this board's.
They are applied but stay off.

The board DTS enables UART0, UART1 with the Bluetooth child, I2C2 with the
Chestnut PMU, SPI1 with the touchscreen and Wi-Fi on PCIe port 2. The display
and GPU nodes from `t8020.dtsi` stay unused.

## USB

- ConfigFS gadget with ACM (`ttyGS0`) and ECM (`usb0`).
- The phone is `172.16.42.1`, DHCP gives the host `172.16.42.2`.
- Dropbear, keys only: `ssh root@172.16.42.1`.
- `usbwatch` rebinds the gadget when the host goes away. Pulling the cable
  and plugging it back in recovers USB, ping and SSH.

## GPU

The XR has the XS Max's G11P. On the XS Max, badapple runs OpenCL, GLES 2 and
3, and Vulkan on it. Nobody has tried it on the XR. It needs:

- `CONFIG_DRM_AGX=y`;
- `armfw_g11p.im4p` wrapped as `gfxf` with a ticket that lists it, sent with
  `firmware` before `bootx`;
- badapple's `m1n1-0012` (GPU rails) and `m1n1-0015` (hands the firmware's
  `segment-ranges` to Linux);
- `cma=64M` in the boot arguments;
- Mesa with badapple's `mesa-0001-asahi-g11.patch` in the root filesystem.

## To do

- **Display.** Describe `disp0`, the DSI, the Pinot panel and the Chestnut
  backlight in the DTS, then enable `CONFIG_DRM_APPLE_H11P` with simpledrm
  as the fallback. Then page flips and DCS brightness (`0x51`). Start from
  the A13's Pinot work, not the XS Max's `apple,d331p-summit`.
- **Touch.** Raw X was 571 to 4214 and Y 6854 to 13354, the DTS says 6500 by
  14000. Test the corners and fix `touchscreen-size-*` and the orientation.
  Use this phone's `MtCl` instead of the firmware's default calibration. The
  driver polls, the AOP interrupt is unused.
- **Wi-Fi.** Long runs with the cable out, and ASPM (`pcie_aspm=off` now).
- **Bluetooth.** Pairing and audio. `BTRx`, `BTTx` and `BCAL` are unused.
- **Initramfs SError.** m1n1 raises an L2 SError in `tinf_crc32` on some
  initramfs gzip streams, a bigger one with the radio firmware included. A
  reproducible archive avoids it, and the firmware goes over SSH after boot.
  The cause is not known, see
  [DEBIAN_XFCE_NFS.md](DEBIAN_XFCE_NFS.md#initramfs-serror).
- **cpufreq.** Booted with `cpufreq.off=1`. badapple's `linux-0011` runs it
  on the XS Max without pstate 1. Not tried here.
- **Untethered boot.**

## Before each new payload

- Keep simpledrm until the display driver works.
- Keep the ACM shell, ECM, Dropbear and `usbwatch`.
- Upload only, never flash.
- Never mount or write the NVMe namespaces.
- Keep a known good `m1n1-linux.img4`.
- Check boot, `/dev/fb0`, USB reconnect, ping and SSH before the new driver.
