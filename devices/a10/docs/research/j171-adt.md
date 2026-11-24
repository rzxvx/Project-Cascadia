# J171: what the ADT says

The Apple device tree as iBoot (iPadOS 16.4's iBSS) leaves it on an
iPad7,11, read through m1n1's proxy (`u.adt`, 196 KiB, 2026-11-22). The dump
itself stays in `build/adt/` -- it carries the iPad's serial numbers, its
radios' addresses and its sensors' factory calibration. What follows is the
layout, nothing private.

`target-type = J171`, `platform-name = t8010`, board ID 0x1c, chip 0x8010.

## CPU

`cpu0`, `cpu1`: `compatible = apple,hurricane-zephyr`, `clock-frequency`
396 MHz as iBoot leaves it (the lowest Zephyr state: see
[j171-cpu.md](j171-cpu.md)), timebase 24 MHz, `cpu-impl-reg` 0x202050000,
`acc-impl-reg` 0x202f40000 (the cluster's performance controller, which
Linux's apple-cpufreq drives through `/soc/performance-controller@202f20000`).

## Coprocessors (RTKit / IOP)

| node | registers | what |
|---|---|---|
| `aop` (`iop,t8010`) | 0x10800000, 0x10e00000 | the always-on processor, firmware `ipad7caop`. **The motion sensors live behind it**: `accel`, `gyro`, `compass`, `pressure` under `iop-aop-nub`, each with its orientation and factory calibration; also `aop-audio` (the microphones' PDM, voice trigger) |
| `sio` (`iop,t8010`) | 0xae00000 | SmartIO: DMA for SPI3 (the touch controller); clock gates 90, 4 |
| `sgx` (`gpu,t8010`) | 0x1000000 (1 MiB), 0x1d00000 | the GPU (the A10's PowerVR-derived core, run by Apple's firmware); seven performance states 0-1008 MHz, 450-950 mV, clock/power gates 148, 150. [../gpu.md](../gpu.md) |

## Storage: `apcie` and `nvme-mmu0`

`apcie` (`apcie,t8010`) at 0x610000000 (ECAM), four ports, MSI at
0xbffff000. `pci-bridge0` is the NVMe's port: its `s3e` child is the ANS
controller -- `namespaces-present = 07`, `function-boot_from_host` (GPIO
0x57), `nvme-scratch-virt-region` 0xbc000000 + 18 MiB, `ibec-boot = 01`.
`nvme-mmu0`: `sart-region` 0x8bee00000 + 0x1200000 -- the 18 MiB host memory
buffer m1n1 reserves for Linux ([j171-boot.md](j171-boot.md)). Port 3 is the
Wi-Fi module.

## Touch: `spi3` / `multi-touch`

`spi3` (`spi-1,samsung`) at 0xa08c000, DMA through `sio-dma`. Its
`multi-touch` child: `compatible = hid-transport,spi`, `bootloader-type =
HBPP`, `hid-merge-personality = K1F19,6` (the J172's too), GPIOs for reset
(0x1f), the analog and LDO supplies (0x91, 0x1d), chip-select (0x55), the
display syncs (0xb0, 0xb1); the panel's calibration blobs (multi-touch,
firefly, firefly-p100, extended, pressure effect, display offset) are the
same records SysCfg holds. [j171-touch.md](j171-touch.md)

## The radios

`wlan`: `compatible = wlan-pcie-uart,bcm4355`, `module-instance = ruddera`,
`function-reg_on` GPIO 0xe on the PMU, its MAC and an MSF calibration.
`bluetooth`: `compatible = bluetooth,n88`, 3 Mbaud, `function-power_enable`
GPIO 0xd on the PMU, `function-bt_wake` GPIO 0x58, its address and RF
calibration ("taurus"). The firmware the kernel uses is "rudderb"'s
(Pauli1Go's J172 set), and it runs; the J171's own name here is "ruddera".
[j171-wifi-bt.md](j171-wifi-bt.md)

## Power: I2C0, I2C1, HDQ

- `i2c0` 0x3c: `pmu` (`pmu,d2365`), interrupt controller for the board's
  events (`event_name-gpio*`: battery, wlan, bluetooth, codec, pcie0, dock,
  the cover's Hall sensors); `wled-enables` (the backlight). Also there:
  `tristar` (`tristar,cbtl1610`, the Lightning port's mux).
- `i2c1` 0x13: `orion-ic` (`orion,bellatrix`), the USB power source -- the
  device behind Linux's `power_supply usb`, which NACKs on the J171
  ([j171-power.md](j171-power.md)).
- `i2c2` 0x39: `als1` (`als,ct819`), the ambient light sensor, with its
  calibration.
- `gas-gauge`: `bq27540` over HDQ (`function-battery_swi` GPIO 0xad).
- `charger`: the D2365's.

## Display

`disp0` with its DART, the panel 1620x2160 (`display-scale = 2`). iBoot
leaves the framebuffer at 0x8bc2dc000, stride 0x1980 (1632 pixels), 32 bpp;
Linux uses it through simpledrm. No native display driver yet.
