# P105 backlight — the PMU's LED current DAC (2026-10-03)

The iPad mini 1's backlight is the D1946 PMU's white-LED driver, set over the
same I2C bus as everything else on the PMU. Two wrong turns came first; both
are written down so nobody takes them again.

## What works

| PMU reg | Meaning |
|---|---|
| `0xd0` | brightness bits 10:3 |
| `0xd1` | brightness bits 2:0 (low three bits only) |
| `0xd2` | string enables: the ADT pmu's `wled-enables` (`0x3f`) = on, `0` = off |

Brightness is an 11-bit current DAC, `d0 << 3 | (d1 & 7)`, written `0xd0`
first — `AppleD1946PMU` (kernelcache 12H321) reads and writes it exactly so,
and its `bklE` platform function writes `wled-enables`/`0` to `0xd2`.

The scale is exponential. The ADT's `/backlight` node has the tables:
`iDAC2MilliAmpsTable` steps the current by ~2.5% per unit of the high byte,
and `milliAmps2DACTablePart2` ends at 2047 for `calibratedMaxCurrent`,
22.5 mA (`LmaxProduct` 400 nits). iBoot leaves 1626, about 8 mA, the
140-nit middle (`calibratedMidCurrent`, `LmidProduct`). iOS's own floor,
`LminProduct` 5 nits, is around 590.

Measured at the battery (bq27540 average current, USB attached, idle):

| Setting | Battery current |
|---|---|
| 2047 | −751 mA |
| 1626 (iBoot) | −371 mA |
| 512 | −238 mA |
| 200 | −233 mA |
| 64 | −233 mA |
| 0 | −230 mA |
| `0xd2 = 0` (off) | −230 mA |

So equal DAC steps look roughly equal, and `p105-keys` steps by 64 —
32 steps, the bottom one about 1 nit.

Linux: `drivers/video/backlight/apple-pmu-wled.c`,
`/sys/class/backlight/apple-pmu-wled`, 0..2047, `scale` non-linear;
brightness 0 and `bl_power` off both clear `0xd2`.

## Wrong turn 1: the WLED string registers

`AppleD1946PMU` also has a backlight update that writes `value / 40`, 0..63,
into `0x4b`/`0x4d`/`0x4f`/`0x51`/`0x53` and switches them with bit 6 of
`0x3f`. On this board iBoot leaves `0x3f` in `0x4f`/`0x51`/`0x53`. Writing
them — any level, or bit 6 for off — changes nothing: not the picture, not
the battery current. That code serves other boards.

## Wrong turn 2: DWI

The ADT points `/backlight`'s `function-backlight_update` at `arm-io/dwi`
(`'dwiB'`), a serial link from the SoC to the PMU at `0x3f700000`
(`AppleS5L8940XDWI`). Its protocol, for the record:

- start: `+0x00 = (nclk-div - 1) << 16 | polarity-config` (= 1),
  `+0x50 = str-delay * 24 / nclk-div` (= `0x2ee00`),
  `+0x04 = lockout-us * 24` (= `0x78`), `+0x14 = 0`, `+0x10 = 0x41`;
- `sendWLED` (dwi-version 0): wait for `+0x10` bit 0 to clear, then
  `+0x14 = 0x20000000 | (v & 3) << 9 | v >> 2`, `+0x10 = 0x71`
  (an alternative pair at `+0x40`/`+0x44` is used without the wait);
- buck voltages go out on `+0x20`/`+0x24`/`+0x30` as
  `0x30000000 | 1 << (24 + buck) | value << (8 * buck)`.

The block is clocked (PMGR gate 126, `0x3f101134`) and the transactions
complete, but the brightness does not move — not even after setting the
PMU's `0xd3` the way iOS's PMU driver does at start (`0x14` → `0x10` →
`0x18`). iOS also sets bit 0 of `0x1c` right before writing
`dwi-buck-control` (5) to `0x3b`; that probably enables the PMU's DWI
receiver, and with it DWI control of buck0/buck2 (CPU and SoC voltage).
Not tried: plain I2C does the job, and nothing here needs the bucks touched.

`tools/debug/pmureg.c` reads and writes single PMU registers from the device
(no i2c-tools on the rootfs). Never point it at `0x29` or `0x42`.
