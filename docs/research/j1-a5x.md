# J1 (iPad3,1, Apple A5X) — the mini's drivers on the iPad 3 (2026-10-24)

The iPad 3 Wi-Fi (iPad3,1, board J1AP, S5L8945X) runs Linux 6.12 from RAM
with this repository's drivers: the gadget console and network over the
30-pin cable, SSH, the framebuffer iBoot left, multi-touch and Wi-Fi. iOS
9.3.5 stays on the NAND and is what starts it. `dts/j1ap.dts` is the tree it
boots with. This is what differs from the mini and what was measured on one
iPad; issue #22 asked about this machine.

## What carries over unchanged

The A5X is the A5's two Cortex-A9s with a bigger GPU and memory, and most of
arm-io is where the mini has it: AIC `aic,1` at `0x0f200000`, UART0 at
`0x02500000` (IRQ 21), DWC2 at `0x06100000` (IRQ 11, 14 endpoints, fifo
`0x820`), the OTG PHY at `0x06000000`, PMGR at `0x0f100000`, the watchdog
tick at `0x0f103020` (IRQ 4), the PL310 at `0x3e000000`, and GPIO at
`0x0fa00000` (`gpio,s5l8945x`, IRQ 119, 28 ports of 8). The AIC, UART,
gadget, PHY, tick, GPIO, I2C and SPI drivers bind as they are, by their
s5l8940x compatibles.

## Boot chain

The same method as the mini's (a signature-patched iBSS, a plaintext iBEC,
then a loader and the kernel), with iOS 9.3.5 (13G36) images and the public
iPad3,1 13G36 keys. Here it starts from a jailbroken iOS with kloader rather
than from checkm8: kloader puts the patched iBSS in RAM, iBSS waits on USB,
and the host sends iBEC, `setenv loadaddr 0x80000000`, the loader with the
zImage and DTB behind it, and `go`.

What does not carry over from the mini's chain:

- **Link addresses.** iBSS is at `0x34000000` (SRAM). iBEC and iBoot are
  linked at `0xBFF00000`, 1 MiB under the end of a 1 GiB window from
  `0x80000000`; the mini's 512 MiB iBEC is at `0x9FF00000`.
- **iBoot-2817, not 2261.** Both images are iBoot-2817.60.2, so the mini's
  auto-go offsets do not apply. `iBoot32Patcher` finds `verify_shsh` in both
  (iBSS file offset `0x66fe`, iBEC `0x1a08c`).
- **`go` is not a raw jump.** iBoot-2817's `go` bounds-checks `[0x80000000,
  0x83f00000)`, parses an IMG3 there and only then calls the jump helper
  (`0xbff1d89c`), which quiesces the machine and `blx`es the parsed entry. A
  raw loader fails the parse and `go` returns. Eight bytes at iBEC file
  offset `0x119e` (the parser call) become a store of `0x80000000` into the
  entry slot and a branch to the existing jump path; the helper itself is
  untouched, so the entry must be ARM, not Thumb. The helper takes USB down
  before the `blx`, so the kernel has to bring the gadget back
  (`console=ttyGS0`).
- **The loader** (ARM, at `0x80000000`) stops the watchdog through iBoot's
  mapping (`0x0F103020`; the physical `0x3F103020` is the wrong address while
  the MMU is on), cleans L1 and the PL310, turns the MMU off and enters the
  zImage at `0x80008000` with `r0=0`, `r1=~0` and `r2` = the appended DTB.

## Memory

1 GiB (`hw.memsize` 1034944512); the ADT's memory node is zero. The DTS
starts RAM at `0x80200000`: with the no-map CPU reset page at `0x80000000`
inside the memory node, it became memblock region 0, and
`prepare_page_table` clears every kernel mapping past the end of region 0,
the running kernel's included.

iBoot's scanout is at `0xbdb00000`, 2048 x 1536 x 4 (pitch 8192, 12 MiB),
found by searching RAM from the loader; the ADT's framebuffer values are
zero. The memory node stops below it, and `simple-framebuffer` uses it as
is. The mini's framebuffer `memblock_remove` (`0x9f600000`) is ordinary RAM
here and costs about 1 MiB; it was left alone.

## PMGR: 44-byte device-clocks entries

The pmgr node's `device-clocks` entries are 44 bytes on the A5X (a 28-byte
header and a 16-byte name), not the A5's 36. At +0x0c is the power-state
index (register PMGR + `0x1000` + index x 4), at +0x10 the clock-register
index. A gate id is therefore not a register as it is on the mini:

| device | gate id | power state | |
|---|---|---|---|
| SPI1 (touch) | 68 | index `0x41` → `0x3f101104` | `0x200` off, `0x2ff` after `(v & ~0x10f) \| 0xf` |
| FMI0 / FMI1 (NAND) | `0x3b`..`0x3e` | `0x3f1010e0`..`0x3f1010ec` | all `0x2ff` as iBoot leaves them |

The mini's SPI1 register, `0x3f1010e8`, is a NAND controller's here. With
SPI1 off every SPI read returned one repeated junk byte.

## PMU: D1974

`pmu,d1974` at I2C0 `0x3c` (I2C0 is IRQ 18). It takes the same 8-bit
register addresses as the D1946 and binds as `apple,pmu-d1946`, without
the GPIO controller (its registers differ, below) or `system-power-controller`
(the D1946's power-off is not checked against this chip).

- LDO index 0 (touch, `function-power_ldo 0x200`): enable `0x23` bit `0x20`,
  voltage `0x31` (2.50–3.00 V), from AppleD1974PMU's LDO table.
- PMU GPIO *n* is configured at `0x64 + n` (the D1946's is `0x61 + n`).
- Backlight: an 11-bit DAC at `0xd3` (bits 10:3) and `0xd4` (bits 2:0), the
  string enable at `0xd5` (`0x3f` lit). The mini's `0xd0`/`0xd1` are
  something else on this chip. The DTS has no backlight node yet.

## Touch

`multi-touch,j1` on SPI1 (IRQ 29), a plain Z2 bootloader
(`AppleMultitouchZ2SPI`, personality `Z2F13,3`), not the mini's N1. GPIOs
are (port << 8) | pin: reset `0x1407` = 167, `enable_cs` `0x907` = 79, ATTN
57, `enable_download` `0x1406` = 166.

What it took, against iOS's own byte trace of the bootload (`mtlog`, which
works on 13G36 unchanged):

1. SPI1 powered at `0x3f101104` (above).
2. `enable_download` driven around reset and held through the bootload,
   **active-low**: driven high or left alone, the chip never enters HBPP.
   This is the `download-gpios` property and `HXT_IOC_SET_DL`, in a separate
   PR for `apple-z2.c`.
3. The 32 kHz clock: PMU GPIO 0 (`0x64`) in iOS's mode, `0x51` (bits 7:5 =
   `010`), not driven high. As a high output the bootload completes and the
   firmware never answers after EXECUTE.

Then calibration (to `0x1000c000`) before firmware, as iOS 9 does; a `1f 01`
status of `4969` is normal. With the scan started (`bf 9f 0b 0b 02`, `af
00`), `HXT_IOC_READY` hands the chip to the driver and touches arrive on
`/dev/input/event0`. The panel is 14745 x 19660 (0.01 mm, the J1's 8-byte
`d9` report). The J1 boot tool is not part of this change.

## Wi-Fi

A BCM4330 B2 on HSIC port 2 of a second USB complex (`usb1-complex`,
`0x3f109000`; its EHCI at `0x36c00000`, IRQ 162), not on the mini's EHCI.
iBoot leaves that complex off and there is no PHY driver for it yet, so
it is brought up by hand from a running system:

1. PMGR: USB1-REG `0x3f1010b4` (the PHY's parent; without it the PHY reads
   bus junk) and USB1-EHCI `0x3f1010a4`, `0x20f` → `0x2ff`.
2. USB_CTL `0x3f109000` = `0x84` (HSIC on, no auto gating).
3. The PHY at `0x36800000`, as iOS's setPower(1) does it: +`0x08` |= `0x3f`,
   +`0x60` = `0x2e0`, +`0x00` = `0x0f`, +`0x04` = `0x11`, +`0x40` = `0x349`,
   +`0x44` = `0x733`, 20 µs, +`0x08` &= ~`0x02`, 1 ms, +`0x08` &= ~`0x08`.
4. EHCI +`0xb0` = `0x2` (HSIC port mask); REG_ON is PMU GPIO 3 (`0x67` =
   `0x0b`).
5. Bind `36c00000.usb` to ehci-platform again (the probe at boot fails -110
   while the domain is off).

The boot ROM then enumerates as `0a5c:bd19`. The firmware is iOS's
`4330b2/mike.trx` with its NVRAM (`mike.txt`) and a `macaddr=` appended,
as for the mini. Unlike the mini's 4334, after DL_GO the 4330 keeps the
boot ROM's connection and answers DL_GETVER with `0xa123` on it within a
second; the bus reset brcmfmac does for the 4334 loses it (-71 from then
on). The brcmfmac change is a separate PR. With it, wlan0 scans 2.4 and
5 GHz and joins WPA2.

## NAND

Read-only so far: Cascadia's PPN tool, with the bus power-state set-feature
taken out (the J1 runs SDR). Four Hynix PPN 1.6 chips, 32 GiB, 8 KiB pages;
page reads return status `0x40` and repeat byte for byte.

## Not done

- CPU1 (`maxcpus=1`).
- The GPU and a display driver: the panel is eDP (`edp@9500000`) fed by
  CLCD through a highland-park scaler at `0x0b020000`, not MIPI-DSI.
- A PHY driver for the second USB complex.
- A backlight node, charging (the D1974's input limit), the battery, audio,
  Bluetooth, NAND writes.
