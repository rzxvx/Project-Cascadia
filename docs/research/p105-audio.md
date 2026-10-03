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

## CS35L19 init (AppleCS35L19Amp, text `0x80af4de0`)

Register helpers through the class vtable: write `+0x4d4 (spk, reg, val)`,
update bits `+0x4ec (spk, reg, mask, val)`, read `+0x4dc`, block write
`+0x4e0`. The ADT's `speaker-config` is 20 bytes per speaker:

    ff 26 02 70 6a 80 30 1e 02 04 0c 8a 8a 10 74 03 05 00 00 00

Per speaker, in order (`0x80af5776..`):

- read reg 5 (revision); if ≥ 0xb0 (errata): `0x00=0x99`, `0x45=0x3f`,
  `0x46=cfg[0x10]`, `0x51=0xff`, `0x59=0x0a`, `0x6b=0x20`,
  [`0x54=cfg[0x12]` if non-zero], `0x00=0x00`
- if `cfg[0x11]`: `0x00=0x99`, `0x66=cfg[0x11]`, `0x00=0`
- `0x0b = cfg[1] & 0x3f`; `0x0c` bits 1:0 = `cfg[2]`

then a broadcast `+0x4d8 (6, 0x80)`, then per speaker:

- `0x07` = a mask built from four calibration words (bits 7..4 for the ones
  that are zero, bit 3 if…) — still to be read closely
- `0x08` bits 7:6 = `0x40`; `0x09 = cfg[0]`; `0x10 = 0x19`;
  `0x0f` bit 4 = 0; `0x11 = 0x1c`; `0x0e` mask `0xcf` = `0xc4`;
  `0x12` bits 7:4 = `cfg[3]`; `0x3a..0x3d` = `cfg[0x0c..0x0f]`

then broadcast `+0x4d8 (6, 0)` and a further loop from `0x80af5a5a` (reg
`0x15` read…) not yet read.

## The NCO (MCLK)

`AppleS5L8940XPerformanceController` vtable `+0x478` (`0x80b8d280`) is
`setNCOFrequency(n, Hz)`, n = 0..6 — the `NCOf` index in the ADT. Each NCO
is four words in PMGR at `0x110 + 16n` (I2S1's, n = 1: `0x3f100120`):

    CTRL = Hz ? 0x90000000 : 0       then wait for CTRL bit 30 to clear
    +4   = 2*Hz                      wait for CTRL bit 9 to clear
    +8   = 2*Hz - Fref               wait for bit 9
    CTRL = 0x90000400, then 0x90000c00 (each: wait for bit 9)

an accumulator divider stepping by `2f` and `2f - Fref`. Before the first
NCO starts the driver enables something through its provider (`+0x354 (1,
0)`; disabled again when all seven are off) — likely the `AUDIO-CLK`
domain (`0x3f100058`, 0 under Linux). `Fref` comes from the provider and is
not read statically; it can be measured: run I2S1 as clock master from the NCO
and time how fast its TX FIFO drains. Under Linux all seven NCOs read
`0x80010000`.

## AE2 is a CPU, and holds the device clocks

`AppleA5AE2` (`/SourceCache/AppleA5AE2/AppleA5AE2-64`): the audio engine is
a Cortex-A5 IOP with firmware (`startCPU(IOSlaveFirmware *)`), five windows
— MCS `0x341e0000`, IRQ `0x341e1000`, ACS `0x341a0000`, WGT `0x341c1000`,
SRAM. iOS 8 keeps its CPU in WFI (`'advW'` =
`AppleA5AE2DeviceClockPutA5InWFIFunction`) and uses the block only for device
clocks: `'advW'` with the ADT index (0 i2s0, 1 i2s1, 5 mca0, 6 mca1) sets a
bit in a mask of devices that need AE2; the first one runs `enableAE2`
(`0x804f4afc`): the provider's clock and power gates on (`+0x354`, `+0x358`),
then **MCS `+0x10 + 4*i` = 1 for i = 0..6** (0..4 when `ae2-version` ≤ 1).
AE2 (id 112) has no PMGR register of its own.

## The i2s-switch encoder (AppleAE2I2SSwitch2, vtable `0x80cfc0c8`)

Own methods: `+0x34c` node count (`[+0x70] + [+0x74] + 7`), `+0x350` index →
name (`dspN`, `dspc`, `audN`, `pinN`, `mcaN`), `+0x354` node type, `+0x358`
register count (21), `+0x364` writes the 21 words to `0x3fa01000`, and
**`+0x360` (`0x80cf950c`) encodes one edge** into a mask array and a value
array (21 words each) — `(signal type lr = 0/1/2, src, dst, index…)`. What
it does, so far:

- destination `pinN` → `reg[N]`: bit 15 on; type 0: source in bits 8..11
  (`dspK` → `4+K`, `audN`/`mcaN` (N > 0) → `7+N`), `aud0` in bits 12..14
  (`7 − pin`); type 1: bits 0..4, `0x10 |` source (`dspK` → `8+K`, `mcaN` →
  `4+N`);
- the port side: `reg[0xa+N]` / `reg[0xb+N]` bits 8..12 (`0x800 + pin<<8` for
  aud, `0x1000 + pin<<8` for mca) and bits 0..4 for type 1;
- `reg[5+N]` for aud, `reg[9+N]` for mca (bits 8..11 or 0..3);
- `reg[0xf]` for `dspc`; type 2 (data?) sets `reg[0x10+N]` bit 0 when an
  aud drives its own pin; `reg[0x14]` bit 0 for the special pin4 → mca0.

The base router (AppleARMIISSwitch, `0x804c09a8`) decides which edges and
which signal types to encode from the route's flag bytes — the ADT's
`0x030303` (mca0 → pin1), `0x300003` (aud1 → pin1), `0x330303` (aud0 → pin0)
— and is the part still to read before the table for a route can be
computed.

## Still open

1. **The i2s-switch's 21 registers** (the router, above). `'i2sR'` lands in AppleARMIISSwitch's
   generic router (`0x804c09a8`): a graph of dsp/aud/mca/pin nodes and per-
   signal edges (the flag bytes `03 03 03`) that computes a 21-word table;
   AppleAE2I2SSwitch2 only copies it to `0x3fa01000`. Linux reads all zero.
2. The NCO's reference clock (to be measured) and what `+0x354` enables.
3. The rest of the CS35L19 sequence (calibration mask, the second loop).
4. CDMA for real playback; PIO into I2S1's FIFO is enough for a first tone.
