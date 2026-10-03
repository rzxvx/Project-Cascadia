# P105 audio — what drives the speakers (2026-10-03, in progress)

Static work on the 12H321 kernelcache and the ADT. Nothing here has been
written to the hardware yet. Live register capture from iOS was tried and
dropped (it would need iOS's kernel-address protection worked around); the
values below come from the drivers' code and the ADT.

## Topology (ADT)

| Part | Chip / block | Control | Data |
|---|---|---|---|
| Speakers (2) | Cirrus **CS35L19** ×2 | I2C0 `0x40`, `0x41`; reset pin 5, alive pin 6, IRQ pins 0x2e / 7 | pin group **pin1** |
| Headphones, mics | Cirrus **CS42L81** | SPI2 | I2S0 → pin0 |
| Bluetooth voice | — | — | I2S1 ↔ pin1 / aud3 |
| Pin routing | `i2s-switch,s5l8942x` (`0x3fa01000`, 4 pins, 3 DSPs) | — | — |
| DMA | **CDMA** (`0x37000000`, 42 channels, 13 IRQs) | — | — |
| Audio engine | **AE2** (`ae2,s5l8940x`, `0x341e0000`…; pclk gates via `advW`) | — | — |

Per port (ADT `function-*`):

| Port | Regs (phys) | NCO (`NCOf`) | Route (`i2sR`) | pclk (`advW`) | CDMA TX / RX (FIFO) |
|---|---|---|---|---|---|
| i2s0 | `0x34190000` | 0 | aud0 → pin0, `0x330303` | 0 | 0x1b `+0x10` / 0x1c `+0x38` |
| i2s1 | `0x34191000` | 1 | aud1 → pin1, `0x300003` | 1 | 0x1d `+0x10` / 0x1e `+0x38` |
| mca0 | `0x34196000` | 6 | mca0 → pin1, `0x030303` | 5 | 0x17 `+0x28` / 0x18 `+0x3c` |
| mca1 | `0x34197000` | 3 | mca1 → pin2, `0x030303` | 6 | — |

The speakers' **MCLK comes from I2S1** (`audio-speaker0 function-mclk_control`
→ `i2s1/audio-bluetooth 'MCLK'`), and MCA0's format descriptor marks it a
clock slave (`desc[0] & 1 == 0`): under iOS, I2S1 clocks pin1 and MCA0 puts
the speaker data on it. For Linux the simpler shape is I2S1 alone on pin1 —
clocks and data — with no MCA at all.

PMGR (scripts/adt-pmgr-map.py), as Linux finds them: `AUDIO` (46) gate
`0x3f10109c` on; `MCA` (47) `0x3f101140` off; `CDMA` (85) `0x3f101078` on;
`I2S0`/`I2S1` (115/116) `0x3f1010a0`/`a4` off; `AUDIO-CLK` (21) `0x3f100058`
= 0. Reading MCA and I2S registers with their gates off does not abort the bus
(`logs/audio/linux-idle.txt`, out of git).

## The I2S block is the S5L8900's

`AppleSamsungI2SController` (i2s-version 3; base ivar `+0x84`) touches
`0x00 0x04 0x08 0x30 0x34 0x3c 0x40 0x410` — the layout openiboot used on
the first iPhone (`openiboot/includes/hardware/i2s.h`, planetbeing/iphonelinux):

    0x00 CLKCON   bit 0: power
    0x04 TXCON    24, 20 "undocumented" set; 16+ burst; 15 edge; 13 format
                  (0 = I2S); 12 MSB first; 11 LR polarity; 8-10 MCLK divider;
                  5+ sample size (0 = 16 bit); 3+ BCLK per frame; 0+ channel
    0x08 TXCOM    3 transmit, 2 interface enable, 1 DMA request, 0 LRCK
    0x10 TX FIFO  (the CDMA's TX address in the ADT)
    0x30 RXCON, 0x34 RXCOM, 0x38 RX FIFO
    0x3c STATUS   (reads 0x25 at reset)

iOS's configure (`0x80df6f60..0x80df7116`) writes STATUS=1, TXCON, RXCON,
TXCOM=0, RXCOM=0, CLKCON last; its TXCON/RXCON values come from a format
descriptor the framework fills (fields at +0x10..0x13 and +0x1c..0x1f are not
in the ADT blob), so they are to be set from openiboot's bit meanings instead.

## CS35L19 (AppleCS35L19Amp, text `0x80af4de0`)

Both amps answer on I2C0 at `0x40`/`0x41` once **pin 5** (their reset, ADT
`function-reset`) is driven high; both read revision `0xb0` (reg 5), ID
`35 a1 90` at regs 1..3. Reset values: `logs/audio/cs35l19-reset.txt`
(out of git). Pin 6 is `function-alive`.

Register helpers, from their call sites (the class's own vtable starts at
`0x80af6fac`; the slots below are its base class's): `+0x4d4 (spk, reg,
val)` write, `+0x4d8 (reg, val)` write to all amps, `+0x4dc (spk, reg)`
read, `+0x4e0 (spk, reg, ptr, len)` block write, `+0x4ec (spk, reg, mask,
val)` / `+0x4f0 (reg, mask, val)` update bits on one / all.

**MCLK must run first, at 12.000 MHz**: power-on asks
`function-mclk_control` (→ `i2s1/audio-bluetooth 'MCLK'`) for `0x00b71b00`
Hz and logs "could not enable MCLK" otherwise.

Power-on (`0x80af55d4` with state 1), per amp unless "all"; `cfg` is the
ADT `speaker-config` (20 bytes per amp, both
`ff 26 02 70 6a 80 30 1e 02 04 0c 8a 8a 10 74 03 05 00 00 00`):

    rev >= 0xb0:  0x00=0x99 0x45=0x3f 0x46=cfg[16] 0x51=0xff 0x59=0x0a
                  0x6b=0x20 [0x54=cfg[18] if !0] 0x00=0x00
    cfg[17]:      0x00=0x99 0x66=cfg[17] 0x00=0x00   (0 here: skipped)
    0x0b = cfg[1] & 0x3f;  0x0c[1:0] = cfg[2]
    all: 0x06 = 0x80
    0x07 = path power-downs: bit7..4 set for each of the V, I, P, B roles
           absent from the stream, bit 3 if F is absent as well
    0x08[7:6] = 01;  0x09 = cfg[0];  0x10 = 0x19;  0x0f bit 4 = 0
    0x11 = 0x1c;  0x0e (mask 0xcf) = 0xc4;  0x12[7:4] = cfg[3]>>4
    0x3a..0x3d = cfg[12..15]
    wait 10 ms;  all: 0x06 = 0x00;  wait 8 ms
    read 0x15, 0x15, 0x16, 0x16 (latched status)
    0x23..0x2a = cfg[4..11];  0x29[6:0] = 0x14;  0x11 = 0x14

Then the serial port (`0x80af5be0`), per amp. The stream's channels carry
role names per amp n — `spA<n>` the speaker's audio, `spV/spI/spP/spB/spF`
and `spK` the amp's own outputs (V/I sense and the like). Slot widths are
in **bytes**, positions are byte offsets in the TDM frame:

    0x2d..0x31 = byte offset of V, I, P, B, F (0x80 = not sent)
    0x32..0x35 = 32-bit map of the bytes those use (0x32 high)
    0x0d = 3 (0x43 on the first amp in one mode)
    0x08 = rate: 48000 0x49, 44100 0x4b, 32000 0x4d, 24000 0x51,
           22050 0x53, 16000 0x55, 12000 0x59, 11025 0x5b, 8000 0x5d
    0x36 = spA's byte offset | ((7 + width) & 7) << 5;  0xc0 = no audio
    0x38, 0x37 = byte offsets of spK and one more; 0x0c bit 4 / 0x0b bit 6
           enable them

and volume (`0x80af65e0`): all `0x10 = 0x19` muted, else `volume + 0x34`.

For a plain 2 x 32-bit frame, amp 0 on the left slot and amp 1 on the
right, no sense outputs: `0x07 = 0xf8`, `0x2d..0x31 = 0x80`,
`0x32..0x35 = 0`, `0x37 = 0x38 = 0x80`, `0x36 = 0x60` / `0x64`,
`0x08 = 0x49`, `0x10 = 0x34`.

Power-off: all `0x10 = 0x19`, `0x06` bit 7, `0x07 = 0xfe`, wait 1 ms,
`0x06 = 0x87`, wait 4 ms, MCLK off.

## On the hardware: I2S1 by hand (2026-10-03, `tools/audio/i2s1-probe.c`)

The clock tree, from the pmgr node's `device-clocks`:

    AUDIO-CLK (21)  clock 0x3f100058, parents PREDIV0..3      off under Linux (0)
    AUDIO (46)      power 0x3f10109c, parent AUDIO-CLK        on (0x2ff)
    AE2 (112), I2S0..3 (115..118), SPDIF, MCA (47) -- parent AUDIO
      (AE2 has no power-state register of its own; MCA0/1 hang off MCA)
    NCO_REF0/1 (121/122) clocks 0x3f1000c0/c4, parents PREDIV0..3
                    iBoot leaves them on: 0xb0000002, 0x80000001

What happened, in order:

- I2S1's power state (`0x3f1010a4`) goes on with the usual recipe.
- `AUDIO-CLK = 0x80000001` sticks (reads `0xc0000001` right after, then
  `0x80000001`).
- NCO1 takes iOS's sequence without a timeout: `90000c00 00bb8000 ff4d4a00`.
- I2S1's registers take TXCON/TXCOM/CLKCON, and **STATUS counts the TX FIFO
  from bit 7** (`+0x80` a word, 64 deep, a 6-bit field). The FIFO never
  drains — no bit clock — with CLKCON 1, 0x5, 0x11, 0x15 and TXCOM 4, 5, 6, 0xc.
- **AE2's ACS `+0x14 = 1`** (what `enableAE2` writes for I2S1) makes every I2S1
  register read `0x0015006b` and drops writes, with or without the NCO
  running; back to 0 and the block answers again. ACS evidently moves the
  device onto AE2's clock, which is not running. In iOS the device-clock
  function is `AppleA5AE2DeviceClockPutA5InWFIFunction`, which builds an IOP
  endpoint: the AE2's Cortex-A5 probably has to be booted (into WFI) for
  those clocks to run. That is the next thing to read.

## Still open

1. **MCA0's registers** for a 2 x 32-bit TX stream as a clock slave (or
   master). AppleAE2MCA::configure (`0x80cfa9a4`, vtable `0x80cfc868` +0x348)
   computes them from a descriptor the framework builds — `[0]` bit 0 master,
   `[2]` & 3 clock mode, `[4]` slots, `[5]` slot bits, `[8..b]` MCLK ratio,
   `[c..d]` bits per frame, `[10..13]` rate fraction, `[14..17]` /
   `[18..1b]` TX / RX slot masks, `[1c]`/`[1d]` channels, `[1e]`/`[1f]` sample
   bits (helper `0x804acf28`) — and can be run in unicorn like the switch.
2. The NCO's reference clock (to be measured) and what `+0x354` enables.
3. CDMA for real playback; PIO into MCA0's TX FIFO (`0x34196028`) is enough
   for a first tone.
