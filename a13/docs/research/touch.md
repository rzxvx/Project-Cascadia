# Touch on the iPhone 11 (N104)

**Status 2026-10-31: works.** `kernel/extras/apple_hidspi_touch.c` loads the
controller's firmware at boot and gives a multi-touch device; XFCE takes it
as a pointer. This file is the record of how it was found, in order: desk
research first, then the phone.

## The short version

The touch controller is an Apple multitouch chip with **no flash**: on every
power-up the host loads its firmware through the **HBPP** bootloader, then
talks to it over Apple's **HID-over-SPI** transport. Both halves already
exist in Linux for Macs, just not together:

| half | Mac precedent | code |
|---|---|---|
| HBPP boot (firmware, calibration, register writes, execute) | M1 Touch Bar | upstream `drivers/input/touchscreen/apple_z2.c`, fed by asahi-installer's `asahi_firmware/multitouch.py` (MIT) |
| HID over SPI after boot | M1 MacBook keyboard/trackpad | Asahi downstream `drivers/hid/spi-hid/spi-hid-apple-*.c` (GPL-2.0) |
| the SPI controller (`spi-1,samsung`) | A10 Sandcastle (Corellium) | our A5 copy `ipad-mini-linux/patches/files-sandcastle-port/drivers/spi/spi-s5l8940x.c` (GPL-2.0) |

The firmware and the whole boot recipe are in **one IPSW file that can be
fetched on its own**, `Firmware/N104_Multitouch.im4p` (583 KB, tag `mtfw`,
not encrypted) — no rootfs DMG needed.

## ADT: `/arm-io/spi1/multi-touch`

| property | value | meaning |
|---|---|---|
| `compatible` | `gen-mt-decider` | |
| `agm-primary` / `agm-secondary` | `AppleHIDTransportDeviceSPI` / `AppleGenericMultitouchHIDSPI` | HID-over-SPI, not Z2 frames |
| `bootloader-type`, `hbpp-version` | `HBPP`, `0x103` | |
| `hid-merge-personality` | `C1FC2,2` | key into the firmware plist |
| `multi-touch-calibration` | `syscfg/MtCl` | iBoot fills it from syscfg; IPSW ADT has only the string |
| `function-reset` | `<&gpio 'GPIO' 197 0x10001>` | AP GPIO 197 |
| `function-enable_cs` | `<&gpio 'GPIO' 95 0x101>` | AP GPIO 95 (also spi1 `function-spi_cs0`) |
| `interrupt-parent`, `interrupts` | `aop-gpio`, `<34 ...>` | **AOP** GPIO 34 (`aop-gpio` names mask 34 "touch") |
| `function-power_ana` | `<&display-pmu 'dpLE' 0x0302>` | Chestnut (I2C2 @0x27) LDO; probably already on for the panel |
| `function-power_ldo` | `<&smc-pmu 'pKW4' 'rBK3' 0x05020000>` | SMC key write |
| `function-clock_enable` | `<&smc-pmu 'pKW4' 'gP00' 0x08000000>` | SMC key `gP00` (= `smc_gpio` 0) |

`/arm-io/spi1`: `spi-1,samsung`, `spi-version` 1, reg `0x235104000` (+0x4000),
AIC irq 445, DMA via SIO (ignore, PIO first), PMGR device `SPI1`.
PMGR power state: **`0x23b0801f8`** (dtsi offset `0x801f8`; same formula gives
APCIE `0x80378` and ANS2 `0x80388`, matching Hoolock's t8030.dtsi). I2C2 is
`0x801b0`. AOP GPIO controller: `0x24a820000` (same as M1's `pinctrl_aop`).

Hoolock's t8030.dtsi has **none** of these: no spi, no i2c, no `pinctrl_aop`,
four PMGR domains only.

## The firmware plist (`N104_Multitouch.im4p` payload)

IOKit-XML (ID/IDREF), keys `C1FC2,2`, `C1FC2,3` and their `-AdcDump` /
`-Playback` variants. `C1FC2,2` (firmware `0x0292`, `ExpectedVersion` 658):

| # | type | address | what |
|---|---|---|---|
| 0 | Binary | `0x0` | 62 764 B — ITCM (armv5 "Cumulus" core) |
| 1 | Binary | `0x400000` | 17 312 B — DTCM |
| 2 | Property | `0x40dd14` | `multi-touch-calibration` (`MtCl`, 2156 B, checked: u16 version 5); plist carries a default payload |
| 3 | RMW | `0x10001110` | `0x554c` |
| 4 | Binary | `0x1000c000` | `N104_C3-demod-lutchebyshev-0x0292.bin`, 2424 B |
| 5 | RMW | `0x10001110` | `0` |
| 6–10 | RMW | `0x10003060/5c/58/00/518` | `0x1725, 2, 6, 3, 1` — the same FLL / ref-clk-div / const-cal / clk32 registers as the A5's N1 |
| 11 | RequestCalibration | | `1f 01` |
| 12 | RMW | `0x10003400` | `1` — execute |
| 13 | Config | | the HID-over-SPI settings below |

asahi-installer's `plist_to_bin_touchbar()` turns exactly these step types
into the `Z2FW` file that `apple_z2.c` uploads (`0x3001` blob packets,
`0x1e33` register writes, `0x1f01` calibration request). It skips Config.

Config (after boot):

- `Protocol` HIDSPI, `Always Powered`, `Power Off On Reset`, `Boot Timeout` 500 ms
- `SPI Config`: mode 0, `Clock Period` 125 (ns -> 8 MHz), CS delay 300 000 (ns?)
- `HIDSPI Config`: `Transfer Length` 512, `Padding Length` 4,
  `Device Interface Id` 0xd0, `Inter Transaction Us` 250, `Inter Stage Us`
  200, `Busy Delay Ms` 2, `Max Input Report Length` 4096
- `Device Tree Overrides`: `power-sequence` = power_ana, power_ldo,
  clock_enable (10 ms); `reset-sequence` = function-reset
- one HID interface "grape": the touch report is **one opaque vendor
  blob** (~1171 bytes, no report ID; usage page 0x0d / 0xff00:0x23), plus
  stats reports 248 and 114. `DefaultMultitouchProperties`: `parser-options`
  80, `SupportTapToWake`.

Asahi's spi-hid-apple matches this transport: info device `0xd0`,
`SPI_RW_CHG_DELAY_US 200 /* 'Inter Stage Us'? */`. Difference: Macs use
256-byte packets, the iPhone says 512.

The touch blob is Apple's multitouch frame (as on the A5 and Mac trackpads:
header, finger count, fixed-size finger records). Its exact layout here has
to come from live frames.

## Plan

1. **Kernel plumbing (desk):** DT nodes for `spi1` + `ps_spi1`,
   `pinctrl_aop`, I2C2 if Chestnut needs it; port the S5L SPI driver to
   t8030 (or confirm spi-apple's layout fits — the register map is the same
   family); `spidev` for a userspace first pass; m1n1: copy
   `multi-touch-calibration` into the FDT for n104 (`dt_set_multitouch`
   only knows the j293/j493 Touch Bars).
2. **Userspace HBPP boot over spidev (phone):** power, reset, then the HBPP
   check (`1a a1` -> status words, as on the A5), then the 13 steps.
   Fast loop over NFS, no kernel rebuilds.
3. **HID-over-SPI (phone):** device info from `0xd0`, HID descriptor, frames.
4. **Kernel driver:** HBPP boot (apple_z2 code) + spi-hid-apple transport
   (512-byte packets) + a frame parser -> input-mt touchscreen.
   Firmware file made by our `inputs` step from `N104_Multitouch.im4p`.

Open questions for the phone: does our iBSS-only boot fill `MtCl` in the
ADT; what the SMC writes `gP00`/`rBK3` mean (read the keys first); is the
Chestnut rail already on; which SPI register layout answers.

## On the phone, 2026-10-31 (first boot with spi1)

- `spi-apple-s5l` binds: `CLKCFG 0x1 CONFIG 0x4018 STATUS 0x30 PIN 0x6
  CLKDIV 16` -- the registers take the writes, the Samsung layout fits. The
  PMGR chain SIO_BUSIF -> SIO -> SPI_P -> SPI1 and I2C2 all come up on.
- Pins as iBoot left them: AP 92/93/94 in peripheral function 1 (SPI1
  SCLK/MOSI/MISO, `0x0087622x`/`0x000742a0`), CS AP 95 a GPIO output, reset AP
  197 a GPIO output **low** (`0x00472202`), interrupt AOP 34 an input, low.
- **The controller is powered and in its HBPP bootloader already** (iBoot
  leaves it so; reset low does not hold it): `spix /dev/spidev0.0 ...`
  answers with the HBPP status words `1f01 4879 4bc1 4ad1 4969`, one transfer
  late. A run of `+64` zero bytes returned
  `1f01 4879 4bc1 4ad1 4969 4879.. 4c39 0203 0000 0005 4c39 0206 ...
  50 32 32 50 32 5a 5a ff..` ("P22P2ZZ"). The very first transfer after boot
  read `ef be ad de 80 00..` (0xdeadbeef). clkdiv 1 works as well as 16.
- No calibration in the FDT: m1n1's copy did not land (`apple,z2-cal-blob`
  absent); its log line is on the DCSD UART. The firmware plist carries a
  default `MtCl` payload, which is enough to boot the firmware.
- SMC: `gP00` and `rBK3` are function keys (flags 0xf0). A plain read of
  `gP00` returns junk, `rBK3` -> -EIO; they are read with a command word
  (`apple_smc_rw`, as macsmc-gpio does). The ADT's values follow the Mac
  encoding: `pKR4 gP09 0x02000000` is CMD_INPUT, so `gP00 0x08000000` is
  command 8, `gP0x 0x10000` resets and `0x800000` enables. Not needed while
  iBoot leaves the controller on.

Next: the userspace HBPP loader -- the 13 steps of `C1FC2,2` over spidev.

## The firmware boots (2026-10-31)

`tools/mtfw.py` makes `build/n104-mtfw.bin` from the plist (byte for byte
what asahi-installer's converter makes, minus its crash on a Config without
`Init Payload`); `tools/peek/hbpp-load.c` sends it over spidev
(`hbpp-load -c n104-mtcal-default.bin /dev/spidev0.0 n104-mtfw.bin`, the
calibration being the plist's default). Log: `touch-hbpp-load-1.txt`.

What the bootloader wants is the **A5's** (MTSPIBootloader_Z2) way, not the
Touch Bar's (apple_z2):

- data packets start with `18 e1`: `18 e1 30 01 <nwords BE16> <addr: low
  half, high half, each BE> <hcsum BE16> <words, each as two BE halves>
  <sum32 the same way>`, at most 0x3f0 bytes of payload;
- after each packet and a 20 ms wait, ATN_ACK `1a a1` in a chip-select window
  of its own; the two bytes clocked in are the status: `4bc1` data accepted,
  `4ad1` register written, `4969` after the calibration request.

Without the `18 e1` and with the ATN_ACK in the same window (apple_z2's way)
the register writes are taken but no data packet is (no `4bc1`).

All 87 data packets `4bc1`, all 8 register writes `4ad1`. Right after the
EXECUTE write the controller sends:

    ef be ad de | 20 d0 0000 0000 0400 | a0 80 00 00

= 4 bytes of padding (`Padding Length 4`), then an Apple SPI-HID packet:
flags 0x20 (read), device 0xd0 (the info device), offset 0, remain 0,
length 4, data `a0 80 00 00` -- exactly `spi_hid_apple_booted` in Asahi's
spi-hid-apple. **The firmware runs and speaks the Mac SPI-HID transport.**
(The first transfer after Linux boots also starts `ef be ad de`: iBoot may
leave a booted firmware behind; not looked into.)

Next: the HID-over-SPI exchange -- read packets on the interrupt (512-byte
transfers + 4 bytes padding), ask device 0xd0 for the device list and the
HID descriptor, then frames.

## The firmware does not stay up (2026-10-31, same session)

- `tools/peek/spihid.c`: Asahi's spi-hid-apple protocol from userspace.
  The `booted` packet reads back with a **valid CRC as a 512-byte packet
  after 4 bytes of padding** (`ef be ad de`): transfer size confirmed.
- But the firmware dies within 200 ms of EXECUTE whether or not anything is
  read: a bootloader-only register write (`1e 33 ...`) then ATN_ACK answers
  `4ad1`. Everything read after `booted` is the bootloader's canned answer
  (`1f01 4879 4bc1 4ad1 4969 4879.. 4c39 0203 0000 0005 4c39 0206 .. "P22P2ZZ"`),
  and writes get status `ff ff ff ff`. Same without the calibration, same
  with a 300 us CS setup delay (the plist's firmware `CS Delay` 300000; the
  bootloader's is 0).
- The AOP 34 interrupt line never changes (low throughout: bootloader,
  firmware, idle) -- either not this line, or it needs configuring
  (`function-set_int_config` on aop-gpio 34, `0x6000001 0x4000001`).
- The first read after Linux boots was `ef be ad de 80 00 ..`: a firmware
  packet (flags 0x80) left from before Linux; after it, the bootloader.
- Likely cause: the firmware turns on the analog front end, whose supplies
  the bootloader does not need -- the ADT's power sequence, `power_ana`
  (Chestnut `dpLE 0x0302`), `power_ldo` (SMC `rBK3`, `0x05020000`),
  `clock_enable` (SMC `gP00`, `0x08000000`), 10 ms on the last.
  `gP00 <- 0x08000001` was written (accepted, no visible change).
- SMC GPIO reads with the Mac commands fail here (`gpioget --as-is` -> I/O
  error). The ADT's own reads are `pKR4 <key> <word>` = write-then-read;
  `apple_smc_keys` now does that (`echo 'rBK3 ? 00000000' > key`).
- Chestnut (I2C2, 0x27) answers; 0x00-0x7f, mirrored at 0x80:
  `00: 00 f3 75 a0 1e 0f 0e 0e 0e 0e 0e 02 00 00 00 05`,
  `20: 22 03 00 21 02 23 21`, `55: 8f`, the rest 0. No register map.

### Power state read back (after the rw support, 2026-10-31)

The SMC takes the Mac macsmc commands, as LE u32 (`echo 'gP17 ? 00000001'`
sends 0x01000000 = CMD_OUTPUT):

| key | read | meaning |
|---|---|---|
| `rBK3` (touch `power_ldo`) | `rw 0 -> 1` | **on** already |
| `rLD8` (rose) / `rLDa` (camera) | 1 / 0 | for comparison |
| `gP17` (LCD reset) | OUTPUT 1, PINMODE 3 | Mac encoding works |
| `gP00` (touch `clock_enable`) | every command -5 | pin in a special function (clock out?); `0x08000001` written = iOS's `pKW4 gP00 0x08000000` with 1 |

So power_ldo is not it. `C1FC2,3` (firmware 0x0421, ExpectedVersion 1057;
its power sequence has no power_ldo) behaves the same: `booted`, then the
bootloader. A firmware packet `ef be ad de 80 00..` (flags 0x80, zero
length) sits in the controller at every Linux boot; after it is read, the
bootloader answers -- probably the last word of a firmware that already
died, not a live one.

Left: `power_ana` = Chestnut `dpLE 0x0302` (in both personalities' power
sequence), and whether gP00's clock really runs.

## power_ana, from the 20E247 kernelcache (2026-10-31)

`kernelcache.release.iphone12b` (top-level in the IPSW, im4p not
encrypted, symbols present), kext `com.apple.driver.AppleChestnutDisplayPMU`:

- `AppleDisplayPMU::callPlatformFunction` matches the function word
  `'dpLE'` (0x64704c45) and builds an `AppleDisplayPMUFunctionLDOEnable`.
- Its `callFunction` reads the ADT function's bytes after the phandle and
  word: `[8]` = dpmuLDOSelect, `[9]` = options, `[10..11]` = private data;
  enable = argument != 0. Then `AppleChestnutDisplayPMU::setLDO(ldo, options,
  private, enable)`.
- `setLDO` (ldo <= 3): I2C read of **register 0x05**; LDO bit = `4 << ldo`.
  On enable, if the bit is clear: set it; options bit 0 -> also `| 0x03`
  (refcounted, `+0x9a`); options bit 1 -> also `| (4 << sage-ldo) & 0xfc`
  (refcounted, `+0x99`). Written back only if it changed. Disable undoes it
  with the refcounts. `handleStart` reads register 5 to seed the refcounts.
- The touch's `function-power_ana` = `<display-pmu 'dpLE' 02 03 0000>`:
  LDO 2, options 3. ADT `sage-ldo` = 0.

So power_ana on = **Chestnut (I2C2 @0x27) register 0x05 |= 0x10** (0x03 and
0x04 are already set). On Linux it reads `0x0f`: **the touch analog LDO is
off** -- the bootloader runs without it, the firmware does not.

### With the analog LDO on (2026-10-31, after a reboot)

- Chestnut reg 0x05 `0x0f -> 0x1f` (`i2cset -y 0 0x27 0x05 0x1f`), then
  `gP00 <- 0x08000001`, then the firmware: **same as before** -- `booted`,
  then the bootloader. Within ~20 ms of EXECUTE a register write is
  answered `4ad1` again.
- Without the ATN_ACK after the EXECUTE write (`hbpp-load -E`) the firmware
  does not start at all (no `booted`): the bootloader acts on a command when
  its ATN_ACK comes. So that ATN_ACK is right.
- Reset AP 197 driven high (`0x00472203`, apple_z2's "released") changes
  nothing: the bootloader keeps answering, the firmware dies the same way.
  Put back low.

So power (both rails as iOS sets them), clock, calibration, personality and
reset are not it, as far as can be seen from outside. Next: what iOS does
around the boot -- AppleHIDTransportSPI / AppleMultitouchSPI in the same
kernelcache (symbols present), from EXECUTE to the first HID request.

## HID-over-SPI on the iPhone, from AppleHIDTransportSPI (2026-10-31)

Kexts with symbols in the 20E247 kernelcache: AppleHIDTransport,
AppleHIDTransportSPI (classes `AppleHIDTransportDeviceSPI`,
`AppleHIDTransportBootloaderHBPP`, `AHTProtocolHBPP13`,
`AppleHIDTransportProtocolHIDSPI`, `HSMTransferDevice`, `HSMDevice`,
`HSParser*`, `HSTransfer`), AppleMultitouchSPI.

**The firmware does not crash.** What killed it: our reads clocked out
zeros, and an all-zero packet has a valid CRC (crc16 of zeros is 0); the
firmware takes it as a command and drops back to the bootloader. Reading
with `0xff` on MOSI it stays up for good and keeps sending its `a0 80`
message.

What is known of the transport now (all tested on the phone):

- A transfer: `[4 bytes padding][512-byte packet]` both ways, full duplex.
  The host's packets need the padding in front too. Packet: `type, device,
  offset16, remain16, length16, data[502], crc16` over the first 510 bytes
  (`HSTransfer::createTransfer`; data area = buffer size - 10, rest zeroed).
- **Status stage**: in the same chip-select window (CS held, spidev
  `cs_change` on the last transfer), 200 us later (`Inter Stage Us`):
  `[padding][4-byte status]` both ways. Words (`HSMTransferDevice::
  handleInterrupt`): `0xd56827ac` OK (`ac 27 68 d5`, Asahi's status_ok),
  `0xf8e5179c` busy, `0x0e4fb139` NAK, `0xb36d5245` error. The device answers
  OK to our packets.
- Transfer types: `0x20` device->host message (`HSParserDeviceInput`, 4-byte
  header, no CRC), `0x10` host->device message (`HSParserDeviceOutput`),
  `0x40` control request/response (`HSParserDeviceControl`, 8-byte header +
  CRC16). Messages are `[a0][message][submessage][0]`: device `0x80`
  attached (`deviceAttached(sub == 1)`), `0x81` detached; host `0x10`
  suspend (sub `0x11`/`0x12`), `0x20` resume.
- Control request (`HSMDeviceInterface::getDescriptor`): `[0x20][subtype]
  [iface][0][len16 = reply buffer size][0 0]` + CRC16, type `0x40`, device
  `0xd0`.
- The device answers device-info / interface-info / HID-descriptor requests
  with a `0x40` packet carrying the same header (`20 01 d0 00`, ...) with
  `rsplen 0, length 0` and a valid message CRC: **understood, but empty**.
  Resume (`a0 20 xx`) first changes nothing.
- The device's packets fail our full-packet CRC once our writes have been
  through: the chip's buffer is shared, the bytes after the payload are left
  from our packets. `spihid -K` answers OK anyway.

Open: why the descriptors come back empty. iOS waits for a second interrupt,
"firmware ready", after `booted` (`interruptAction`: "Device booted. Waiting
for firmware ready interrupt"); `a0 80` has a submessage and iOS branches on
`sub == 1` -- we only ever see 0. Next in the kernelcache: `bootChip`,
`interruptAction`, `HSParserDevice::processTx`/`checkForWork` (what iOS
clocks out when it has nothing to send).

### Enumeration is not needed; what "firmware ready" is (2026-10-31)

Web: nothing on the iPhone flavour; the Mac 256-byte transport is Asahi's
spi-hid-apple, FreeBSD's atopcase, OpenBSD's aplhidev.

- `AppleHIDTransportProtocolHIDSPI::powerHasChanged`: with an
  `Interface Config` in the firmware plist (we have one: interface 0
  "grape" with its `HIDDescriptor`), iOS calls `createInterfacesFromConfig`
  and registers the HID interfaces itself -- **no device/interface/HID
  descriptor requests at all**. `asyncEnumeration` (the requests) runs only
  without a config. So the empty answers to those requests do not matter.
- `AppleHIDTransportDeviceSPI::interruptAction`: after the firmware load,
  "Device booted. Waiting for firmware ready interrupt" just sets a flag;
  the **next interrupt** is "Device ready", and the HIDSPI protocol starts.
  Nothing is sent in between ("Sending initial SPI payload" only with an
  `Init Payload`, which the iPhone plist does not have).
- HID packets (`HSParserHID::createPacket`): message header `[type][report
  id][0][frame][16-bit][len16]` + payload + CRC16; transfer type from the
  message type: `0x10` input -> transfer `0x20`, `0x20` output -> `0x10`,
  others (get/set feature, Asahi's `0x32`/`0x52`) -> `0x40`.
- On interface 0 (packet device `0x00`), SET feature `0xab` = 0 and GET
  feature `0xf8`/`0x72`/`0xab` are all answered with a header-only `0x40`
  packet with a **valid** CRC (`52 ab 00 ...`, `32 f8 00 ...`, length 0).
- `spihid -u` prints/saves only packets that differ from the one before;
  a 15-minute background capture with touches is the next test.

## TOUCH FRAMES (2026-10-31)

With the firmware loaded (`hbpp-load -A -c n104-mtcal-default.bin`) and
`spihid -u -K` reading (0xff on MOSI, status stage OK), touching the screen
gives **input reports on interface 0**: packet type `0x20`, device `0x00`,
CRC good, ~120 frames/s while a finger is down. 2042 frames from ~10
touches, one and two fingers (raw capture: `build/touch-cap-1.bin`, 516-byte
records). No mode setting was needed (the SET 0xab = 0 sent earlier in the
session may or may not matter -- to be checked on a clean boot).

Message: `10 75 00 <seq> 0000 <len16>` (input report 0x75) + payload + CRC16.
Payload (report 0x75), little-endian:

| offset | what |
|---|---|
| 0 | report id 0x75 |
| 1 | frame counter (8-bit) |
| 2..3 | `20 04` |
| 4..7 | timestamp-ish, +0x10 per frame |
| 8..15 | `00 01 47 97 02 00 00 00` (constant here) |
| 16..19 | bytes of finger records (30 x n) |
| 20..21 | `0x66` |
| 22..23 | number of fingers |
| 24..31 | `10 00 00 00 00 00 00 00` |
| 32.. | finger records, 30 bytes each (then a fixed trailer) |

Finger record = `struct apple_z2_finger` (Linux apple_z2.c, the A5's too):
`id, state, u16, s16 x, s16 y, rel x, rel y, tool major, tool minor,
orientation, touch major, touch minor, 2 x u16, pressure, multi`.
States: 3 down, 4 touching, 7 lifted, 0 gone. Ids 2, 3, ... per finger.
Range seen: x 722..6218, y 2364..12316 (physical units, probably 1/100 mm:
the panel is ~64 x 138 mm); the corners still need measuring for the
screen mapping.

Next: a kernel driver -- HBPP boot (A5-style packets) + this transport
(padding, status stage, 0xff reads) + report 0x75 -> input-mt.

## Kernel driver (2026-10-31)

`kernel/extras/apple_hidspi_touch.c` (+ DT patch 0003): power (Chestnut
LDO, SMC gP00), firmware back to the bootloader with an all-zero packet,
HBPP load (`apple/n104-mtfw.bin`, calibration `apple,z2-cal-blob` or
`apple/n104-mtcal.bin`), then HIDSPI polled from a kthread (8 ms with a
finger down, 25 ms otherwise; ~5 % of a core idle), report 0x75 ->
input-mt "Apple iPhone touchscreen". The firmware files come from
`a13/cascadia inputs` and go into the initramfs at `pack`.

First boot: `touch analog LDO on`, `calibration: 2156 bytes
(apple/n104-mtcal.bin)` (m1n1 found no panel MtCl), `firmware running` at
3.2 s, `/dev/input/event1`, udev tags it `ID_INPUT_TOUCHSCREEN`. Corners
(raw): top-left 310,13420; top-right 5638,13172; bottom-right 5770,228;
bottom-left 214,228 -> `touchscreen-size-x = 6000`, `size-y = 13650`,
`touchscreen-inverted-y`.

udev was not running at all (BusyBox init): X's libinput saw no input
devices. `a13-init` starts udevd now; eudev is in the base rootfs.

Confirmed by the user in XFCE on 2026-10-31: the pointer follows the finger,
taps click.

Open: the interrupt line (polling costs ~5 % of a core idle); what the boot
really needs (LDO, gP00 and the 0xab mode were all set when it first worked);
a calibration finer than four corners; suspend/resume.
