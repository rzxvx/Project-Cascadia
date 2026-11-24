# J171: touch

## The chain

The touch controller ("K1F19,6", Apple's Z2 family) on SPI3 at 0xa08c000;
SPI3's DMA goes through **SmartIO** (`sio`, an IOP of its own); a PMGR clock
for the controller. Pauli1Go's drivers: `apple-sio` (its firmware, its DMA
endpoint), `apple-sio-dma`, the T8010 SPI controller, the T8010 touch clock,
and `apple_z2` -- which loads the controller's firmware through its HBPP
bootloader and reads Apple's HID-over-SPI frames.

Kernel options the fragment needed beyond Pauli's defconfig list: without
`DMADEVICES` (the menu `APPLE_SIO_DMA` sits in), `APPLE_SIO` and
`INPUT_TOUCHSCREEN` the drivers were simply not built -- no error, no touch.

## SmartIO's firmware

`apple/t8010-smartio.bin`: `AppleT8010SmartIO`'s `__DATA.__firmware` in the
iPadOS 18.7.9 kernelcache (`kernelcache.release.iPad7,11_12`, 48529408
bytes, SHA-256 as Pauli1Go's guide gives it), 557152 bytes at file offset
0x2a46000. Missing, the kernel waited 60 s for a sysfs helper (now
`FW_LOADER_USER_HELPER` is off: it fails at once) and SmartIO's probe
failed; present, `apple-z2` went on to ask for its own.

## The controller's firmware

`apple/dfrmtfw-j172-k1f19-6.bin` (the name the shared iPad 7 DT asks for),
made by Paul Praschl's **makez2fw** (GPL-2.0-or-later, pinned) from:

- `Firmware/J171_Multitouch.im4p` of iPadOS 18.7.9 -- byte-identical to
  `J172_Multitouch.im4p` (both 2205551 bytes, the same SHA-256), payload
  2205526 bytes: the MTFW HBPP sequence;
- this iPad's **SysCfg** (`./cascadia a10 syscfg`: NVMe namespace 3 read
  over ssh, checked to be read-only, 128 KiB, `gfCS`).

makez2fw picks `Kona-J171-master.bin` (224644 bytes) from the MTFW by
itself, and places the calibrations from SysCfg where the HBPP sequence
loads them: `MtCl` (1180 bytes) at 0x100400, `FfCl` (7492) at 0x100a00,
`MtDO` (128) at 0x103700, `FPCL` (1848) at 0x102e00, `MtPc` (12096) at
0x103900, `ETCL` (3820) at 0x106840; around them the HBPP's register writes
(CM4 reset asserted, SafeMode magic cleared, OTP read, image-valid bit, CM4
reset released). The result, 252092 bytes, is private: it carries this
panel's calibration. The ADT's personality is the J172's too (`K1F19,6`).

## In Linux

```
input: iPad7,12 Touchscreen as /devices/platform/soc/20a08c000.spi/spi_master/spi0/spi0.0/input/input0
```

The name is the shared DT's on both boards. ABS_MT_POSITION_X 0-1619,
Y 0-2159 (the DT's `touchscreen-size-x/y`, 0x654 x 0x870 -- the screen's
own pixels), resolution 10, tracking IDs 0-65535, `BTN_TOUCH`; in 45 s of
fingers on it, 29 contacts and 188 KB of events. Brought up first by hand
(the firmware copied in, `spi0.0` bound again to `apple-z2`), then from the
initramfs at every boot; it is `event0`, the buttons `event1`.

Users of it: fbkeyboard on the console (found by its name), labwc
(`<touch deviceName="iPad7,12 Touchscreen" mapToOutput="Unknown-1"
mouseEmulation="no"/>`: touches, not a pretend mouse), task-killer (grabbed
while its window is up).
