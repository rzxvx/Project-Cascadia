# P105 charging — the PMU's USB input limit (2026-10-24)

The iPad mini 1 did not charge under Linux: on a host port, a 5 W charger or
an iPad brick alike, the battery went on draining, only slower. The reason,
and almost everything below, is liquidmetal-Hg's (issue #23): read off the
D1946 PMU on his own mini, and out of the 12H321 iBEC and kernelcache. His
driver, `apple-pmu-charger.c`, is what charges the iPad now.

## Why it drained

D1946 register `0x3d` is the USB input current limit. iBEC hands over with it
at `0x02`, about 100 mA, and nothing in Linux ever moved it. Charging itself
is on (`0x3f` = `0x3f`, bit 6 clear; the input-suspend pair `0x1b`/`0x1c` =
`0x00`/`0x10`); the input just let almost nothing in.

| Source (his mini) | `0x3d` | Battery |
|---|---|---|
| Linux host port | `0x02` | −386 mA |
| unplugged | `0x02` | −510 mA |
| 5 W USB-C charger | `0x02` | −388 mA |
| the same charger under iOS 9.3.5 | — | +1082 mA (`"usb charger"`, 1000) |

The PMU resets `0x3d` to `0x02` at every plug-in, so a limit never carries
over to the next source.

## How iBoot and iOS set it

- **Encoding:** code = (8 · mA − 600) / 100, 12.5 mA a step from 75 mA; iBEC
  then corrects it: up to `0x48` as is, `0x49`..`0x4a` → `0x48`,
  `0x4b`..`0x6e` → −2, `0x6f`..`0x72` → `0x6e`, `0x73` and up → −4; under
  100 mA it is `0x02`. 500 mA ≈ `0x22`, 1000 ≈ `0x48`, 2100 the most.
- **Ramp:** iBEC's apply step (`0x9ff0c540`) never jumps: +8 codes every
  10 ms, capped at 2100 mA (it patches `usb-input-limit-max` = 2100 into the
  ADT's `charger` node too).
- **Sag:** `0x3d` reading back `0x02` means VBUS sagged and the PMU dropped
  the limit itself; iBEC backs off 8 codes ("limiting USB input current to
  %d mA"), and `AppleD1946PMUPowerSource::handleInterrupt` checks for it too.
- **Policy** (a table in iBEC): a USB host 100 mA, 500 once configured; an
  unknown charger 1000; Apple bricks by their ID 500 / 1000 / 1000 / 2100 /
  2400. A charger is told from a host on the PMU's ADC, channel 6 (`0x43`,
  `| 0x40` starts a conversion), with D+ and then D− routed to it.
- **The same apply step also** writes `0x1b &= 0xbb` (then `| 0x04` only under
  100 mA), `0x1c` bit 7 only under 100 mA, and the charge current: code =
  mA / 40 into `0x3f` and the temperature bands `0x4b`/`0x4d`/`0x4f`/`0x51`/
  `0x53` (`AppleD1946PMUPowerSource`'s set-charge-current, vtable +0x70 --
  what p105-backlight.md's first wrong turn took for the WLED).

## What the driver does

`apple-pmu-charger.c`, a `power_supply` child of the PMU (`charger@3d`,
`/sys/class/power_supply/apple-pmu-usb`):

- `0x3d` is the only register it writes (one helper refuses any other); it
  never reads `0x01`..`0x09`, which clear on read.
- Online is `0x0a` bit 6, polled every second.
- It ramps to its target iBEC's way and backs off 8 codes on a sag. On
  Cascadia the limit then stays there until the cable comes out (the
  driver's own was 60 s); a cable out and in within the 1 s poll looks like
  a sag.
- Above 45 °C on the battery gauge it drops to 100 mA, back under 40 °C.
- The driver's default is to write nothing (`armed`, a module parameter):
  it logs what it would set, and disarming puts back what it found at probe.
  Cascadia's image arms it from the bootargs (`apple_pmu_charger.armed=1`)
  with a target of 1000 mA; more by hand (`max_input_ma`, then
  `input_current_limit`).

| | `0x3d` | Battery |
|---|---|---|
| his mini, host port, 500 mA | `0x22` | +87 mA |
| his mini, 5 W charger, 1 A | `0x48` | +615..626 mA for 44 min, 44 → 55 %, 29 °C at most |
| ours, a Mac's USB-C port, before | `0x02` | −238 mA |
| ours, 500 mA | `0x22` | +253 mA |
| ours, 1000 mA | `0x48` | +620..752 mA, no sag, the USB link and NFS root up |

After `poweroff` iOS came up charging normally on his mini: nothing sticks.

## Not done yet

- **Telling a charger from a host.** The status registers are the same for
  both (`0x0a`/`0x0b`/`0x0e` = `6b`/`05`/`58` plugged, `03` or `23`/`00`/`00`
  not); iBEC's ADC measurement of D+ and D− would let the driver take 2.1 A
  from a big brick by itself, and stay at 500 mA on a weak host port.
- **Plug events without polling:** plug-in sets `0x02` = `06`, `0x05` = `c0`,
  `0x09` = `6a`; unplug `0x05` = `40`, `0x07` = `40`, `0x09` = `e8` (event
  registers: they clear on read, and the PMU's interrupt owner should read
  them, not this driver).
- **Restart** (`reboot` powers off today): iOS's `_shutdownGated` restart
  path (`0x80ecec80`) writes `0xaf = (0xaf & 0x2f) | 0x90`, `0xa1 = 0`, then
  `0x2b` to `0x80`. Untried here.
