# P105 motion sensors — the accelerometer, and the screen turned with it (2026-10-23)

The iPad mini 1's accelerometer is an ST LIS3DSH on the SoC's third I2C
controller. Linux reads it through IIO (`st_accel`, which did not know the
part and does now), and `autorotate` turns the XFCE screen with it. No
capture from iOS was needed: the ADT names the bus, the address and the kind
of chip, and the chip's WHO_AM_I names the part.

## What the ADT has

`arm-io/i2c2` (`i2c,s5l8940x`, reg `0x3400000`, AIC interrupt `0x14`, the
same block as i2c0) carries three chips; `arm-io/i2c1` has the compass.

| ADT node | compatible | address | read from the chip |
|---|---|---|---|
| `i2c2/accelerometer` | `accelerometer,ap3dsh` | `0x1d` | WHO_AM_I (`0x0f`) = `0x3f`: **LIS3DSH** |
| `i2c2/gyro` | `gyro,ap3gdl` | `0x6a` | WHO_AM_I = `0xd5` (L3GD20 is `0xd4`, L3GD20H `0xd7`) |
| `i2c2/als` | `als,tsl2581` | `0x39` | ID (command `0x92`) = `0x93`: TSL2581, revision 3 |
| `i2c1/compass` | `compass,akm8963` | `0x0c` | (not read) |

The second word of each `reg` is `0x9c4`. The accelerometer's `interrupts`
are `0x3e` and `0x20` on interrupt-parent `0xf`, the GPIO controller (the
gyro's `0x21` and `0x3d`, the light sensor's `0x2f`). Nothing here uses them.

At Linux's start the bus is ready: its clock gate (PMGR `0x3f101120`, as for
i2c0 and i2c1 next to it) reads `0x2ff`, on, and its pins
(`0x3fa00300`/`0x304`) are `0x621`, muxed like i2c0's. The driver for i2c0
(`i2c-s5l8940x.c`) takes it as it is: "Hx I2C core rev 1, clkdiv 15".

**The interrupt.** i2c2's `interrupts = <20>`, the ADT's `0x14`, arrives:
9 interrupts while the three chips were probed. i2c0's was wrong. The DTS
said `<10>`, which the ADT gives `arm-io/scaler`; i2c0's ADT number is
`0x12` (18). No other node uses 18, and every other AIC number in the DTS
matches the ADT. With `<18>` (kernel #314) it arrives,
`APPLE-AIC1 18 ... 33200000.i2c`, where line 10's count had stayed 0. That
was why the driver said the interrupt "never reaches us". One interrupt
comes for each message the driver arms with `IRQMASK`, counted on the iPad:

| on i2c0 | interrupts |
|---|---|
| the boot | 3 |
| a backlight change (writes `0xd0`, `0xd1`, `0xd2`) | 3 |
| a register read (`i2cget 0x3c 0xd0`, write and read as one) | 0 |
| a plain one-message read (`i2cget 0x3c`) | 1 |
| an address nobody answers (`0x50`) | 0 |

A register read goes through the combined path, which polls iBoot's way with
`IRQMASK` at 0, so it raises nothing. The PMU, the backlight
(1500 → `0xd0 = 0xbb`, back to 1626), the touch rails (`0x22 = 0x67`,
`0x23 = 0xe0`) and both amps (DEVID `0x35`, REV `0xb0` at `0x40` and `0x41`)
read as before, with no timeouts. `Err:` did not move during the test.

The driver still polls. The PMU's power-off writes run with interrupts off,
and the combined read has no interrupt to wait on. The interrupt calls the
same handler under the same lock, so whichever comes first does the work, as
on i2c2. The poweroff path is unchanged.

## The LIS3DSH in st_accel

`st_accel` has no LIS3DSH. It is the LSM330's accelerometer on its own, with
the same registers: ODR, BDU and the axes in `0x20`, the range in `0x24`
and the data-ready interrupt in `0x23`. So the new entry
(`patches/tree/0002-st_accel-lis3dsh.patch`) copies the LSM330's, with
WHO_AM_I `0x3f`. Its ODR table follows the LIS3DSH datasheet: `0x07` is
400 Hz, where st_accel's LSM330 table has 200. Measured on the iPad by the
FIFO's fill (FIFO mode, `FIFO_SRC` after ~60 ms):

| `0x20` | samples |
|---|---|
| `0x67` (ODR `0x6`) | 5 |
| `0x77` (`0x7`) | 22 |
| `0x87` (`0x8`) | 31 and overrun |

`0x7` gives four times what `0x6` (100 Hz) does, so it is 400 Hz.

Lying face up, it reads x, y, z = −250, 220, 16 330: 16 330 × 0.000598 m/s²
(the 2 g range's unit) = 9.77 m/s².

**Reads.** With no interrupt, st_sensors has no trigger and no buffer. A
sysfs read (`in_accel_x_raw`) powers the chip on, waits two of its samples,
reads and powers it off. At the driver's first rate, 3 Hz, that is two thirds
of a second per axis. `autorotate` sets `sampling_frequency` to 100 Hz:
20 ms per axis.

## The mount matrix

Each ADT `orientation` is three words, one row each, with the row's three
entries as signed bytes:

| chip | rows |
|---|---|
| accelerometer | (−1, 0, 0), (0, −1, 0), (0, 0, −1) |
| gyro | (0, −1, 0), (1, 0, 0), (0, 0, 1) |
| compass | (−1, 0, 0), (0, −1, 0), (0, 0, 1) |

The accelerometer's matrix is minus the identity: it takes the chip's axes
to iOS's, and iOS reads gravity, −1 g on z face up. IIO reads the push
against gravity, +1 g up (the reading above, face up). So Linux's matrix is
the identity: the chip's axes are the screen's, x right, y up (away from
Home) and z out of the glass. `dts/p105ap.dts` says so in `mount-matrix`.
Checked by turning the iPad: y ≈ +16 000 upright with Home down, and the
picture comes up the right way in all four turns.

## autorotate

`tools/desktop/autorotate.sh`, started with each XFCE session (an autostart
entry), reads the three axes about three times a second with the mount
matrix applied. When the iPad lies flat (less than half of the push in the
glass's plane), it does nothing. Otherwise the larger of x and y decides,
once the other is under 2/3 of it (34 degrees off the axis):

- y > 0: normal
- y < 0: inverted
- x < 0: left (RandR's: the picture's top at the panel's left)
- x > 0: right

The same answer twice running calls `rotate` (M41: the KMS plane turns, and
libinput's matrix turns the touch).

**The rotation lock** is the file `/run/rotation-lock`, gone at the next
boot. Two things make and remove it:

- the panel's button, next to the keyboard's (`rotation-lock toggle`);
  autorotate adds the button at the first login and sets its icon to the
  lock's state;
- Home held for 0.8 s (`p105-keys`, which has the buttons to itself). The
  backlight dips once for on and twice for off.

The side switch was taken (it picks what the volume buttons set), and Hold
puts the screen out. That left Home.

## Without a new kernel

`tools/sensors/dtmod` adds the bus and its chips to a running kernel with an
of_changeset, as `tools/audio/dtmod` does for the speakers. With the modules
built in the kernel tree (build/linux symlinked to it, mounted in
`cascadia-build`):

```bash
make -C build/linux ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- M=/cascadia/tools/sensors/dtmod modules
make -C build/linux ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- M=drivers/iio/common/st_sensors modules
make -C build/linux ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- \
    KBUILD_EXTRA_SYMBOLS=/cascadia/build/linux/drivers/iio/common/st_sensors/Module.symvers \
    M=drivers/iio/accel modules
```

Then, on the iPad: `insmod p105-sensors-dt.ko`, then `st_sensors`,
`st_sensors_i2c`, `st_accel` and `st_accel_i2c`. A kernel from
`./cascadia build` has the bus in its DTB and the driver built in.

## Left

- The gyro: `0xd5` is in no st_gyro table. It is probably an L3GD20 under
  Apple's part number. To check its registers before adding it.
- The light sensor: Linux's `tsl2583` driver takes `amstaos,tsl2581`.
  It needs `CONFIG_TSL2583` and a node, then automatic brightness.
- The compass (AK8963 on i2c1): `CONFIG_AK8975` is built in, but there is
  no i2c1 node.
- i2c0's interrupt is settled (above). Transfers driven by the interrupt
  instead of polling would still need a polled fallback for power-off.
