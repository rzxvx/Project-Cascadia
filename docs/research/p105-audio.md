# P105 audio — what drives the speakers

**State (2026-10-16): the speakers play, as an ALSA card.** `iPadMini`:
MCA0 fed by CDMA, both CS35L19 amps, 32/44.1/48 kHz, S16_LE/S32_LE;
`aplay`, `speaker-test`, SDL all play, a continuous tone without a gap. The
drivers are `sound/soc/apple/s5l-audio.c` and `sound/soc/codecs/cs35l19.c`
(the last section). Before the drivers the same was done by hand from
userspace (`tools/audio/spk-tone.sh`).

The hardware facts below come from three places: the 12H321 kernelcache
(iOS 8.4.1's drivers, read statically and **run in unicorn** where reading
would invite mistakes), the ADT, and experiments on the iPad under Linux
(`tools/audio/audmmio.c`, an MMIO script runner). Live register capture from
iOS was tried and dropped (iOS's kernel-address protection).

## Topology

| Part | Chip / block | Control | Data |
|---|---|---|---|
| Speakers (2) | Cirrus **CS35L19** ×2, close kin of the CS35L33 | I2C0 `0x40`, `0x41`; reset GPIO 5, keep-alive GPIO 6, IRQ pins 0x2e / 7 | pin group **pin1** |
| Headphones, mics | Cirrus **CS42L81** | SPI2 | I2S0 → pin0 |
| Bluetooth voice | BCM4334 | — | pin3 |
| Pin routing | `i2s-switch,s5l8942x` (`0x3fa01000`, 4 pin groups, 3 DSPs) | — | — |
| DMA | **CDMA** (`0x37000000`, 42 channels, 13 IRQs) | — | — |
| Audio engine | **AE2** (`ae2,s5l8940x`): a Cortex-A5 IOP plus the audio clock switches | — | — |

Per port (ADT `function-*`):

| Port | Regs (phys) | NCO (`NCOf`) | Route (`i2sR`) | AE2 index (`advW`) | CDMA TX / RX (FIFO) | IRQ |
|---|---|---|---|---|---|---|
| i2s0 | `0x34190000` | 0 | aud0 → pin0, `0x330303` | 0 | 0x1b `+0x10` / 0x1c `+0x38` | 0x63 |
| i2s1 | `0x34191000` | 1 | aud1 → pin1, `0x300003` | 1 | 0x1d `+0x10` / 0x1e `+0x38` | 0x64 |
| mca0 | `0x34196000` | 6 | mca0 → pin1, `0x030303` | 5 | 0x17 `+0x28` / 0x18 `+0x3c` | 0xa8 |
| mca1 | `0x34197000` | 3 | mca1 → pin2, `0x030303` | 6 | 0x21 `+0x28` / 0x22 `+0x3c` | 0x66 |

The board (820-3243 boardview) names the four pin groups after their users:
`I2S0_CODEC_ASP` (pin0: MCK, BCLK, LRCK, DOUT, DIN), `I2S1_SPKAMP` (pin1, the
same five), `I2S2_CODEC_XSP` and `I2S3_BT` (no MCK). Under iOS the speakers'
data come from **MCA0**, and their **MCLK from I2S1** (`audio-speaker0
function-mclk_control` → `i2s1/audio-bluetooth 'MCLK'`, 12.000 MHz).

### The pads (GPIO block, `0x3fa00000 + 4*pin`)

iBoot leaves every audio pad in peripheral mode. Found by sampling every pin's
input bit while a port ran:

| Group | MCK | frame clocks, data out | DIN |
|---|---|---|---|
| pin0 (codec ASP) | 76 (`0xa20`) | 77–79 (`0x620`) | 80 (`0x220`) |
| pin1 (speakers) | **81** (`0xa20`) | **82** (LRCK), 83, 84 (`0x620`) | 85 (`0x220`) |
| pin2 (codec XSP) | — | 87–89 | 90 |
| pin3 (BT) | — | 92–94 | 95 |

## Clocks

### The PMGR side

From the pmgr node's `device-clocks` (`scripts/adt-pmgr-map.py`):

    AUDIO-CLK (21)  clock 0x3f100058, parents PREDIV0..3     0 under Linux
    AUDIO (46)      power 0x3f10109c, parent AUDIO-CLK       on (0x2ff)
    MCA (47)        power 0x3f101140, parent AUDIO           off
    I2S0/I2S1 (115/116)  power 0x3f1010a0/a4, parent AUDIO   off
    AE2 (112), MCA0/1 (113/114): no register of their own
    NCO_REF0/1 (121/122)  clocks 0x3f1000c0/c4                iBoot: 0xb0000002, 0x80000001

Power states take the usual recipe (`(v & ~0x10f) | 0xf`, wait for bits 7:4
to match 3:0). `AUDIO-CLK = 0x80000001` is what the experiments ran with.

### The NCOs (MCLK generators)

`AppleS5L8940XPerformanceController::setNCOFrequency(n, Hz)` (`0x80b8d280`),
n = the port's `NCOf` index. Each NCO is four words at PMGR `0x110 + 16n`:

    provider clock gate 0 on (the pmgr node's clock-gates: NCO_REF0)
    CTRL = Hz ? 0x90000000 : 0       wait for CTRL bit 30 to clear
    +4   = 2*Hz                      wait for CTRL bit 9 to clear
    +8   = 2*Hz - Fref               wait for bit 9
    CTRL = 0x90000400, then 0x90000c00 (each: wait for bit 9)

**Fref = 342 MHz**, measured: with a guessed 24 MHz the NCO came out 14.25×
fast (MCA0's FIFO drained at 0.4448 words per nominal Hz, against the
0.4453 that 342 MHz predicts). 342 MHz is PLL3's VCO (24 × 171 / 4 =
1026 MHz) divided by 3. With it, MCA0 runs at **48002 frames/s** for a
48 kHz setting. Idle NCOs read `0x80010000`.

### AE2: the per-port clock switches

AE2 (`ae2,s5l8940x`) maps five windows: MCS `0x341e0000`, IRQ `0x341e1000`,
ACS `0x341a0000`, WGT `0x341c1000`, SRAM `0x34000000` (0x30000). Its
Cortex-A5 runs an IOP firmware embedded in the kernelcache
(`IOP_s5l8940xae2_firmware`, "EmbeddedIOP for s5l8940xae2"); iOS 8 only
keeps it in WFI for the device clocks (`AppleA5AE2DeviceClockPutA5InWFIFunction`).
**Linux needs none of that.** What matters, per port index i (the `advW`
argument: i2s0 0, i2s1 1, mca0 5, mca1 6):

- **ACS `+0x10 + 4i`: the port's register clock gate, 1 = gated.**
  `setDeviceClock` (`0x804f4ee0`) writes 0 to turn a port on and 1 to turn it
  off; `enableAE2` (`0x804f4afc`) writes 1 to all seven the first time. Under
  Linux they read 0, so every port's registers answer. (A gated port reads
  `0x0015006b` everywhere and drops writes — the "AE2 wall" an earlier session
  ran into was this gate, written the wrong way round.)
- **MCS `+0x10 + 4i`: the port's kernel clock source** (found by experiment):
  0 = stopped, **1 = its own NCO**, 2 = 24 MHz, 3 = 12 MHz (both from the
  crystal; bits 2+ ignored). Linux finds `0 1 2 3 4 1 1`: mca0 (i = 5) on its
  NCO. Changing MCS `+0x24` changes MCA0's rate at once; MCS `+0x14` does
  nothing visible for I2S1.
- MCS `+0x04` bit 8 follows mca1's clock; `0x00ff0003` there is AE2's CPU
  start, which nothing here needs.

## The I2S block (S5L8900 heritage, i2s-version 3)

The layout openiboot used on the first iPhone:

    0x00 CLKCON   bit 0 power/run; bit 4 + bits 7-8: clock from "nclk" instead
                  of the NCO (slave/derived modes); 0x34 = stop (v3), bit 1
                  reads back "stopped"
    0x04 TXCON    bit 7 MCLK = BCLK (ratio 1), else bits 8-10 = ratio/2 - 1;
                  bits 3-4 BCLKs per frame; 5-6 sample size; 11, 12, 15
                  polarity/format; 20, 24, 25 set by iOS
    0x08 TXCOM    bit 1 DMA request, bit 2 interface on; iOS (v3): 0x01f80006 |
                  0xa00 | watermark << 14 (48 kHz: 0x01feca06)
    0x10 TX FIFO
    0x30 RXCON, 0x34 RXCOM, 0x38 RX FIFO
    0x3c STATUS   TX FIFO level in bits 7-12 (0x80 a word); write 1 to flush;
                  bit 0 toggles with LRCK while running

What iOS writes, computed by running its configure and start
(`AppleSamsungI2SController`, vtable `0x80df80c8`, configure `+0x348` =
`0x80df6d78`, start `+0x350` = `0x80df7408`) for the bluetooth template at
48 kHz: `STATUS=1, TXCON=0x03000081, RXCON=0x1000, TXCOM=0, RXCOM=0,
CLKCON=1`, NCO = 1.536 MHz (= BCLK), then `TXCOM=0x01feca06`. **With exactly
these values I2S1 runs** (16 words drain in 165 µs, 48 kHz stereo); with
openiboot's divider settings (`0x01100301` and kin) it never makes a bit
clock — which is what stalled the first attempt. **CLKCON = 1 alone puts the
NCO out on the port's MCK**, TX on or off.

## The MCA block (AppleAE2MCA)

`tools/audio/mca-emu.py` runs AppleAE2MCA's configure (`0x80cfa9a4`) and start
(`0x80cfb08c`) in unicorn on a format descriptor and prints every register
access. Descriptor (32 bytes, the ADT's `audio-data` children carry
templates): `[0]` bit 0 clock master, `[1]` format flags (the speakers use
0x12), `[2]` clock mode, `[4]` slots per frame, `[5]` slot width (0 = the
sample width), `[8..b]` MCLK, `[0x10..13]` rate as kHz/1, `[0x14]`/`[0x18]`
TX/RX slot masks, `[0x1c]`/`[0x1d]` channels, `[0x1e]`/`[0x1f]` sample bits.

Master, 2 × 32-bit slots, 32-bit samples, 48 kHz, flags 0x12 — what the
speakers get:

    +0x44 = 0x301, wait until it reads 0     reset: every register to default
    +0x10 = 0;  +0x1c = 0xd;  +0x30 = 0xd
    +0x08 = 0x0000   (32 - sample bits, TX | RX << 8; 16-bit: 0x1010)
    +0x20 = 0x002f0004, +0x34 = 0x002f0004   TX/RX FIFO thresholds
    +0x14 = 0;  +0x24 = 3 (TX slots);  +0x38 = 3 (RX slots)
    +0x18 = 0x208 (TX config), +0x2c = 0x10a (RX config)
    +0x10 = 1;  +0x40 = 0xb0003000;  +0x0c = 0x0f401007 (frame: slot width
            in bits 20-23, 1 = 16, 4 = 32; master in the low bits)
    NCO = 6.144 MHz (MCLK = 2 × BCLK)
    start TX: +0x18 |= 1

`+0x28` is the TX FIFO. **`+0x40` is the status:** bits 0-6 the TX FIFO
level (it reaches 0x32, so 64 words deep), bit 10 level below the `+0x20` threshold, bit 12 TX
underflow (write 1 to clear), bits 26/28 the RX side. iOS's interrupt handler
reports frame error (`+0x10` bit 1), RX overflow/underflow (bits 29/28) and
TX overflow/underflow (bits 13/12).

## The i2s-switch (AppleAE2I2SSwitch2)

21 words at `0x3fa01000`, version `0x20001` at `+0xffc`, all zero under Linux.
`tools/audio/i2sswitch-emu.py` runs iOS's router (AppleARMIISSwitch
`0x804c09a8`) for the ADT's routes. Its results and what experiments added:

- `reg[pin]` (pin1 = `+0x04`): bit 15 drives the pin group at all; **bits 0-4
  its frame clocks = 0x10 | source** (aud1 → 1, mca0 → 4: 0x11 and 0x14 both
  put LRCK/BCLK on pads 82/83); bits 12-14 its data source (7 = mca0).
- **`reg[0x10 + pin]` bit 0 (`+0x44` for pin1): the pin's MCK, from audN**
  (pad 81 toggles with I2S1's MCLK).
- `reg[9]`: mca0's clock input (1 = from pin1) when MCA0 is a clock slave.

| Route | Registers |
|---|---|
| speakers as iOS runs them: I2S1 master + MCA0 TX slave | `reg1 = 0xf000`, `reg9 = 1` |
| MCA0 TX as clock master | `reg1 = 0xf014` |
| aud0 → pin0 (codec) | `reg0 = 0x9010` |
| **what Linux uses** | **`reg1 = 0xf014`, `reg[0x11] = 1`** |

## CS35L19 (AppleCS35L19Amp, text `0x80af4de0`)

The register map is the **CS35L33's** (Linux `sound/soc/codecs/cs35l33.h`):
the reset values match it register for register (`0x06 0x85`, `0x07 0xfe`,
`0x0a 0x55`, `0x0f 0x14`, `0x11 0x04`, `0x12 0x90`, `0x2d-0x31 00 03 02 05 06`,
`0x36 0x40`, `0x37 0x03`, `0x38 0x04`); ID `35 a1 90`, revision `0xb0`. So:
`0x06` PWRCTL1, `0x07` PWRCTL2 (monitor and SDIN/TDM power-downs), `0x08`
CLK_CTL, `0x09` boost peak current, `0x0a` protection, `0x0b`/`0x0c` boost,
`0x0d` serial port (ALIVE_RATE in bits 0-1), `0x0e` ADC, `0x0f` DAC,
**`0x10` digital volume**, `0x11` class D (bit 3 calibration), `0x12` amp
gain, `0x15`/`0x16` interrupt status, `0x2d-0x38` TDM slots.

- **Volume `0x10`**: 0.5 dB steps, two's complement around 0 dB: `0x00` =
  0 dB, `0x18` = +12 dB, `0x34` = −102 dB, `0x19` = mute (what iOS writes to
  mute). iOS's control 0x131 runs 0..228 (−102 dB..+12 dB) and is written as
  `value + 0x34`. (The first audible test was silent because 0x34 — iOS's
  volume 0 — is −102 dB.)
- **Status**: `0x15` bit 5 ALIVE_ERR, bit 4 serial-port clock error, bit 3
  MCLK error; `0x16` bit 0 power-down done. Without MCLK the amps stay powered
  down (`0x16 = 0x01`) whatever else is right.
- **MCLK must run first, at 12.000 MHz** (`0x08 = 0x49`: MCLKDIV2, 48 kHz):
  iOS's power-on asks `function-mclk_control` for `0x00b71b00` Hz and logs
  "could not enable MCLK" otherwise.
- **Keep-alive**: the board wires GPIO 6 to the amps' `KEEPALIVE` input; iOS
  flips it from a 20 ms timer (`0x80af54e4`, `[+0xfc] ^= 1`, re-armed with 20).

Power-on (`0x80af55d4`), per amp unless "all"; `cfg` is the ADT
`speaker-config` (20 bytes per amp, both
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

Then the serial port (`0x80af5be0`), per amp. Slot positions are byte offsets
in the TDM frame:

    0x2d..0x31 = byte offset of V, I, P, B, F (0x80 = not sent)
    0x32..0x35 = 32-bit map of the bytes those use (0x32 high)
    0x0d = 3 (0x43 on the first amp in one mode)
    0x08 = rate: 48000 0x49, 44100 0x4b, 32000 0x4d, 24000 0x51,
           22050 0x53, 16000 0x55, 12000 0x59, 11025 0x5b, 8000 0x5d
    0x36 = spA's byte offset | (bytes - 1) << 5   (RX_AUD: offset, depth)
    0x37, 0x38 = 0x80 (supply and alive slots off)

For a plain 2 × 32-bit frame, amp 0 on the left slot and amp 1 on the right,
no sense outputs: `0x07 = 0xf8`, `0x2d..0x31 = 0x80`, `0x32..0x35 = 0`,
`0x37 = 0x38 = 0x80`, `0x36 = 0x60` / `0x64`, `0x08 = 0x49`.

Power-off: all `0x10 = 0x19`, `0x06` bit 7, `0x07 = 0xfe`, wait 1 ms,
`0x06 = 0x87`, wait 4 ms, MCLK off.

## The recipe that plays (tools/audio/spk-tone.sh)

1. `AUDIO-CLK = 0x80000001`; MCA (`0x3f101140`) and I2S1 (`0x3f1010a4`)
   power states on.
2. MCLK: NCO1 = 12 MHz; I2S1 `STATUS=1, TXCON=0x03000081, RXCON=0x1000,
   RXCOM=0, CLKCON=1`.
3. Switch: `reg1 = 0xf014` (MCA0's clocks and data on pin1),
   `reg[0x11] = 1` (I2S1's MCK on pin1).
4. Keep-alive: GPIO 6 flipped every 20 ms from here on.
5. Amps: GPIO 5 low, then high; power-on and serial port as above.
6. MCA0: NCO6 = 6.144 MHz; reset and configure (2 × 32 bits, master); TX on.
7. Volume `0x10` (0x00 = 0 dB); feed `+0x28` whenever `+0x40` & 0x3f is low.

Heard on the device 2026-10-16: a 1 kHz square wave from both speakers.

## CDMA (cdma-version 3, `0x37000000`)

From iBEC's own `drivers/apple/cdma/cdma.c` (`dma_execute_async`
`0x9ff01c54`, `dma_continue_async` `0x9ff01f7c`, the interrupt handler
`0x9ff022cc`; iBoot's code is linear and reads well), the register names
from AppleCDMA's error log, and experiments:

    global +0x00/+0x04  channel clocks on (a bit per channel; write 1)
           +0x08/+0x0c  channel clocks off
           +0x10/+0x14  channel clocks status (Linux finds 0xfffffe18, 0x7ff)
    channel n at 0x1000 * n (n = the ADT's dma-channels number, 1..42):
      +0x00 CSR  bit 0 run; 1 abort (write 2); 3 completion interrupt enable;
                 16 running; 18 error, 19 done, 20 CIR (all write-1-to-clear);
                 24-27 iBoot's 2
      +0x04 DCR  request line in bits 16-21 (the ADT's 4th word: mca0 TX
                 0xb), burst in 4-6 (1, 2, 4, 8, 16, 32 -> 0..5), width in
                 2-3 (1, 2, 4 bytes -> 0, 1, 2), bit 1 memory to device.
                 **Bit 0 locks DCR until the next boot.**
      +0x08 DAR  the device address, 24 bits (0x196028 for MCA0's FIFO; the
                 window's top comes with the request line)
      +0x0c DBR  bytes left; a multiple of 16, at most 0x7ffffff0 (negative
                 as a signed number: ERR 4)
      +0x10 MAR  the memory address being read
      +0x14 CAR  the command being run (32-byte aligned)
      +0x18 ERR  4 = bad length, 0x100 = a register written while running
      +0x20..+0x2c  the command being run, as fetched

Commands are 32 bytes in memory: `{NXT, CMD, ADDR, LEN}` (+16 unused);
`CMD = 3 | 2 << 24`, `| 0x100` on iBoot's last one, 0 stops. Start: CSR =
0x001c0000 (clear), 2 (abort), DCR, DAR, DBR, CAR, then CSR = 0x021c0009.
**A ring works**: the last command's NXT pointing back at the first, DBR
0x7ffffff0, the channel runs until the count is spent (~90 minutes of 48 kHz
32-bit stereo) and paces itself on the request line -- measured 24 frames
every 0.5 ms into MCA0. It raises nothing while it runs, so a period clock
has to come from elsewhere: MAR.

The ADT's dma-channels word 3 (`0x00040002`) is AppleCDMA's transferControl:
width in the low half (2 bytes), burst in the high (4); `0x00040004` is the
32-bit variant.

MCA0's `+0x20` low byte is the TX FIFO level at which it asks for DMA (iOS:
4, i.e. ~40 µs of margin; the FIFO is then filled to ~0x30). Linux uses
0x10.

## The kernel drivers

`sound/soc/apple/s5l-audio.c` (`apple,s5l8942x-audio`, `CONFIG_SND_SOC_APPLE_S5L_AUDIO`):

- maps MCA0, I2S1, AE2's MCS and ACS, the i2s-switch, the NCOs and CDMA
  (`reg-names`); power states through two `apple,s5l8940x-pmgr-clk-gate`
  nodes (`mca_ps`, `i2s1_ps`);
- registers **the amps' MCLK as a clock** (`#clock-cells = <0>`): enabling it
  sets NCO1 to 12 MHz, programs I2S1 as iOS does and opens the switch's
  reg 0x11; its parent is I2S1's power state;
- the PCM: prepare sets NCO6 to 128 fs, resets and configures MCA0 (the
  registers above, 16-bit samples with `+0x08 = 0x1010`), points pin1 at it
  (`reg1 = 0xf014`) and builds a ring of one command per period; trigger
  starts the CDMA channel (`apple,dma-channel = <0x17 0xb>`), then MCA0's TX;
  an hrtimer at half a period reads MAR for the position and calls
  `snd_pcm_period_elapsed`, and restarts the channel when its count runs out;
- registers the card on a child device: the amps take MCLK from this
  device, so it has to finish probing first, and the card has to wait for
  them (with both in one probe, fw_devlink deadlocks).

`sound/soc/codecs/cs35l19.c` (`cirrus,cs35l19`, `CONFIG_SND_SOC_CS35L19`):

- regmap with single reads and writes (the chip does not step the address
  within a transfer: a bulk read of the ID returns `35 35 35`);
- the shared reset GPIO is taken non-exclusively and left released; the
  amp with `cirrus,keepalive-gpios` flips it every 20 ms while powered;
- a DAPM output driver runs iOS's power-on (with MCLK enabled first) and
  power-off; `set_tdm_slot` gives each amp its 32-bit slot, hw_params the
  rate code; "Speaker Volume" is reg 0x10 (0..228 = -102..+12 dB).

DT: `dts/p105ap.dts` (`mca_ps`, `i2s1_ps`, `audio`, `spk_left`/`spk_right`
on I2C0). Userland: alsa-utils is a base package of the image
(`scripts/add-base-packages.sh`) and `initramfs/etc/asound.conf` (dmix as
`default`) is copied onto the NFS root at every boot;
`tools/audio/audio-setup.sh` installs the package on an older NFS root. Verified 2026-10-16 with the drivers as modules (on the
running kernel, nodes added by `tools/audio/dtmod`): `speaker-test` and a
continuous 10 s 440 Hz `aplay` (S16) heard clean, two `aplay`s at once
through dmix, SDL3's ALSA driver running the card.  Then on the flashed image
(kernel #304, the drivers built in): the card comes up at boot (both amps
probe at ~2.5 s), a tone through `default` (dmix, S16_LE 48 kHz, CDMA
channel 0x17) heard clean, and the volume buttons step the volume over
the whole range (`tools/p105-keys.c`, the side switch picking volume or
backlight).  And with the userland in the image (kernel #307), in
a RAM-root boot with nothing installed by hand: `aplay` of a continuous
tone through `default` (dmix) heard clean; `speaker-test`'s clicks every
~3 s are its own channel switches, cutting the sine mid-wave.

## Still open

1. The headphone codec (CS42L81 on SPI2, I2S0 → pin0) and the microphones.
2. The amps' V/I sense back on MCA0's RX (the `0x2d..0x35` slots), and the
   speaker protection iOS builds on it.
3. A CDMA dmaengine driver for everything else (SPI, NAND).
