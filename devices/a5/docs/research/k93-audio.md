# K93 audio — the iPad 2's CS42L63 behind I2S0

**State (2026-11-10):** Linux plays to the speaker and the headphones and
records from the built-in microphone and from a headset's
(`sound/soc/apple/s5l8940-audio.c`, `sound/soc/codecs/cs42l63.c`, iOS's
sequences and input-gain tables compiled in through
`tools/audio/k93-seq2c.py`).  When a plug goes in the driver does what iOS's
Mikey code does -- measures the plug's mic contact, picks the switch's
wiring, biases the mic -- and playback and capture move to the plug by
themselves.  All confirmed on the iPad, by ear and by measurement: section 5
has what the hardware showed beyond the emulation, section 6 the headset's
microphone.

**Earlier (2026-11-09, evening):** the playback path alone: the card probes,
a tone plays at the right rate, the codec, I2S0, the switch and GPIO 39 in
exactly the state predicted below, the speaker heard.  NCO Fref 342 MHz is
confirmed.

**Originally (2026-11-09): research only, nothing run on the iPad.** Everything
below comes from the iOS 8.4.1 (12H321) K93 kernelcache
(`build/k93/firmware-12H321/kernelcache.12H321.k93.macho`) and the K93 device
tree, read statically and — wherever a register value is quoted — **computed
by running iOS's own code in unicorn**. The iPad runs iOS 6.1.3; its drivers
may differ in details (one hint: the ADT's `disable-auto-mic-bias-clamp` is
not referenced anywhere in the 8.4.1 kernelcache).

Legend used throughout: **[E]** emulated (the value is what iOS's code
produced), **[C]** read in the code (certain, not run), **[A]** by analogy
(CS42L73 datasheet/driver or the iPad mini's measured behaviour), **[?]**
open — the hardware experiment that settles it is given.

## Tools (all offline, `devices/a5/tools/audio/k93-*`)

Run from `devices/a5` with
`docker run --rm --user "$(id -u):$(id -g)" -v "$PWD":/cascadia -w /cascadia cascadia-build python3 tools/audio/<script>`.

| Script | What it runs / prints |
|---|---|
| `k93-cs42l63-emu.py [--sequences FILE]` | AppleCS42L63Audio's methods (probe, init, routing, volume, mute, power-down, inputs) with the I2C nub, GPIO functions, MCLK and the audio controls stubbed; logs every I2C transaction. `--sequences` regenerates the module below |
| `k93-cs42l63-sequences.py` | **the machine-readable sequences** (generated; `python3 tools/audio/k93-cs42l63-sequences.py` prints JSON) |
| `k93-i2s-emu.py [--nclk HZ]` | AppleSamsungI2SController setPower/configure/start/stop/setMCLKEnable on the audio0 template at 48 and 44.1 kHz |
| `k93-i2sswitch-emu.py` | AppleAE2I2SSwitch1 + AppleARMIISSwitch's router for i2s0's route |
| `k93-ts3a8235-emu.py` | the TS3A8235 headset switch's state encoder and setState |
| `k93-adt-audio.py` | the audio ADT nodes decoded (function-* arguments, dma-channels, templates, PMGR registers); host python, no docker |

The emulators follow `tools/audio/mca-emu.py`/`i2sswitch-emu.py`: the whole
kernelcache is mapped, the object is built the way iOS's `start()` leaves it,
the kernel proper is not run (calls return 0, `IOSleep` and friends are
logged), and stand-ins log what the code asks of other drivers.

## Topology

| Part | Chip / block | Control | Data / clocks |
|---|---|---|---|
| Codec | Cirrus **CS42L63** (ID `42 a6 3x`, rev `b0`) | I2C0 `0x4a` | ASP ↔ I2S0 via the i2s-switch, pin group 0; MCLK 6 MHz from I2S0 |
| Codec reset | PMU (D1946) **GPIO 13** | `function-reset`, active low | — |
| Speaker | external amplifier fed from the codec's **line out 2** (`lineout2-spkr`), 6 dB (`spkr-gain-db`) | enable = SoC **GPIO 39** (`0x0407`, `function-spkr_mute`, low = muted) | — |
| Headphones | codec HP A/B | — | jack ground/mic via the **TS3A8235** (I2C0 `0x3b`) |
| Codec IRQ | SoC GPIO 15 (`interrupts = <0xf 1>`) | headset detect / buttons | — |
| Mics | internal = **DMIC B** (`dmicb-intmic`), headset = **MIC2** (`mic2-extmic`) | — | ASP SDOUT |
| Ports | i2s0 `0x34190000` (codec), i2s1 → highland-park, i2s2 voice, i2s3 reference | AE2 `advW` 0..3, NCO 0..3 | CDMA |

No MCA on this SoC (S5L8940X); the switch is `i2s-switch,s5l8940x`, driven by
**AppleAE2I2SSwitch1**, not the mini's Switch2.

## 1. The CS42L63 as AppleCS42L63Audio drives it

### The driver

`com.apple.driver.AppleCS42L63Audio` (load `0x80c7d000`, text
`0x80c7d4d0`–`0x80c7fc74`), class AppleCS42L63Audio (size `0xe4`, vtable
`0x80c800d8`), a subclass of AppleCSI2CAudio ← AppleI2CAudio ←
AppleEmbeddedAudio (`com.apple.driver.AppleEmbeddedAudio`). Two helpers:
AppleCS42L63Mikey (vtable `0x80c80628`, the headset-detect block, an
AppleCSCodecMikey from AppleCSEmbeddedAudio) and AppleCS42L63PWM (unused on
K93: no `pwm` child).

| Method | Address | What |
|---|---|---|
| initFromProvider (vt+0x4c8) | `0x80c7d620` | ADT: mclk_control (+0xc0, absent), hpout_ref_control (+0xc4, absent), spkr_mute (+0xc8), lineout2-spkr (+0xcc), xsp-reference (+0xcd), mic flags (+0xd0: dmicb → `0x10`), at-scale (+0xd4) |
| probe (vt+0x4a8) | `0x80c7d9d0` | reads 0x01..0x03 (auto-increment); wants `42`, `a6`, `3x` |
| initHardware(wake) (vt+0x3a8) | `0x80c7da10` | 0 = cold (with reset), 1 = wake |
| powerDown(really, …) (vt+0x3ac) | `0x80c7e020` | arg 0 → returns `0xe00002d8` (declines) |
| setOutputActive(on) (vt+0x3b0) | `0x80c7e164` | routing; +50 ms when starting on the speaker |
| setInputActive(on) (vt+0x3b4) | `0x80c7e190` | mic path `0x80c7e540` |
| route (non-virtual) | `0x80c7dbd8` | the whole output configuration from the controls |
| setSampleRate(0, Hz) (vt+0x3c4) | `0x80c7e738` | reg 0x11[3:0]; rates table `0x80c7ffb0` (vt+0x3c8) |
| hasToggle / levelRange / selectorItems | `0x80c7e7f8` / `0x80c7e94c` / `0x80c7ecac` | which controls exist, their ranges |
| controlChanged(id, value) (vt+0x488) | `0x80c7ef40` | called after the value is stored |
| HP L / HP R / speaker volume | `0x80c7f028` / `0x80c7f07c` / `0x80c7f0d0` | |
| IRQ handler | `0x80c7d8ac` | reads 0x62, 0x63, then the Mikey's |

I2C access is AppleCSI2CAudio's [C]: write `0x80902004` (vt+0x4d0), read
`0x80902078` (vt+0x4d4), multi-write `0x80902174`, multi-read `0x809020e8`,
update `0x80902200` (`new = old & ~mask | val & mask`, a read then a write).
`init(dict)` (`0x80c7d60c` → `0x80902568` → `0x80901f64`) sets: registers
`0x01`..`0x7e`, address mask `0x7f`, **`0x80` ORed into the map byte for
multi-byte transfers** (auto-increment, as on the CS42L73). Each transfer is
the I2C nub's vt+0x350 `(op 2 write / 1 read, 1 address byte, data)`.

The start path (AppleEmbeddedAudio::start `0x808ff7b0`): look up
`function-reset`, initFromProvider, create the Mikey, then
`initHardware(0)`. The power-stage machine (`0x80900634`) later calls
`powerDown(1, stage==8)` to sleep and `initHardware(1)` to wake.

### Controls

The codec has no ALSA-like controls of its own; AppleEmbeddedAudio creates
IOAudio2 controls (builder `0x80904a38`) and the codec reads them:

| ID | Control (class/selector/scope/element) | Used as |
|---|---|---|
| 0x131 | `levl` `vlme` `outp` 0 — master volume, 0..62 (−62..0 dB, 1 dB) | mixer attenuation `(0x3e − v) & 0x3f` |
| 0x130 | `togl` `dmut` `outp` 0 | forces the master attenuation to `0x3f` |
| 0x132 / 0x133 | element 1 volume / mute — **headphone left** | reg 0x23 |
| 0x134 / 0x135 | element 2 volume / mute — **headphone right** | reg 0x25 |
| 0x137 / 0x138 | element 3 — receiver (needs `earout-rcvr`: absent) | — |
| 0x139 / 0x13a | element 4 volume / mute — **speaker** | regs 0x21/0x22 |
| 0x13b | element 4 data source: `codc` (normal), `voic`, `xref` | |
| 0x16e / 0x16f / 0x170 | input mute / volume / source (`imic`, `emic`) | mic path |
| 0x181/0x182/0x183 | passthroughs `fm2r`/`bb2w`/`w2bb` (0 on an iPad) | |

The codec reports 1 for the mutes in hasToggle (`0x80c7e7f8`), which reads as
their initial value [C/A]: after boot every output is off and iOS's
userland sets volumes and unmutes the selected output. **Selecting an output = unmuting its
element(s) and muting the others**; every change re-runs the whole routing
(`controlChanged` table [C], `0x80c7ef56`: 0x130/0x131/0x133/0x135/0x13b →
route; 0x132 → HP L volume; 0x134 → HP R volume; 0x137/0x139 → speaker
volume; unmuting 0x13a also sets 0x138 = 1 and vice versa).

### Register map, as far as iOS reveals it

Reset values are the live dump (it is exactly the chip's reset state: e.g.
0x0c/0x0e/0x10 = `15`, which iOS overwrites at every init).

| Reg | Reset | iOS writes | Meaning |
|---|---|---|---|
| 0x00 | 00 | `99` … `00` around 0x7e/0x7f | test-register key [E] (same key as the CS35L19's) |
| 0x01–03 | 42 a6 30 | read | device ID; probe accepts byte2 & `f0` = `30` [C] |
| 0x05 | b0 | read | revision; stored (+0xbf), never used [C] |
| 0x06 | 1f | bit 0 cleared at init end, set at power-down; bit 6 set after power-down; bits 4:1 by the mic path | **power control 1**: bit 0 = global PDN [E]; bit 4 cleared for DMIC B, bit 3 for MIC2 [E]; bit 6 unknown [?] |
| 0x07 | 7f | update mask `7c` | **output power-downs (1 = off)** [E]: bit 6 HP A, bit 5 HP B, bits 4:3 line out 2 (the speaker amp's feed), bit 2 earpiece; bits 1:0 left alone on K93 (boards without lineout2-spkr use mask `65`, bit 0 = internal speaker amp) |
| 0x08 | fc | mask `ac` (playback), mask `50` (record) | **power control 3** [E]: bit 7 = ASP playback path (0 while an output stream runs), bit 6 = ASP record path; bits 5/3/2 = voice / XSP-reference paths (cleared only for `voic`/`xref`/`bb2w`); bit 4 mic-side |
| 0x09 | 00 | `&= ~07` | [A] L73: charge pump / Class H |
| 0x0a | 00 | `&= ~8f` | ? |
| 0x0c | 15 | `11` | XSP master-mode clocking [A]: slave, code `11` = MCLK 6 MHz / 48 kHz in the L73's table |
| 0x0d | 00 | bit 7 set (idle) | XSP control: bit 7 = SDOUT tristate [A] |
| 0x0e | 15 | `11` | **ASP clocking: slave** (bit 7 = 0), 6 MHz/48 kHz code [A] |
| 0x0f | 00 | `&= ~10` | ASP control [A] |
| 0x10 | 15 | `91` | VSP clocking: **master** (bit 7), 6 MHz/48 kHz [A] |
| 0x11 | 00 | bits 3:0 | **ASP sample rate** [E]: 1 = 8000, 2 = 11025, 3 = 12000, 4 = 16000, 5 = 22050, 6 = 24000, 7 = 32000, 8 = 44100, 9 = 48000 (the L73's ASRC codes) |
| 0x12 | e0 | `c9`; bits 7:6 cleared at power-down | 3:0 = 9: 48 kHz for VSP/XSP [A] |
| 0x14–0x1b, 0x5e, 0x5f | | mic path | below |
| 0x1c | 20 | `82` | ? (L73-like playback digital control) |
| 0x1d | 00 | `00` HP on / `19` off | **HP digital volume** [E]: `00` = 0 dB, `19` = mute [A: L73 0.5 dB two's complement] |
| 0x1f, 0x20 | 00 | `00` speaker on / `19` off | **line-out-2 (speaker) digital volume A/B** [E] |
| 0x21, 0x22 | 40 | `(v + 0x41) & 0x7f`, `40` = mute | **speaker (LO2 A/B) analog volume** [E], v = 0..62 |
| 0x23, 0x25 | 40 | same encoding | **headphone A (left) / B (right) analog volume** [E] |
| 0x24, 0x26 | 40 | never | (probably line out 1) [?] |
| 0x27 | 05 | `0c` | ? |
| 0x3b | 00 | mask `07`: `03` idle, `01` speaker, `00` HP | bit 0 = HP path muted, bit 1 = all outputs off [E] |
| 0x3c–0x5b | 3f/7f | mixers | below |
| 0x60–0x63 | 9f ff 00 00 | 0x62/0x63 read by the IRQ; 0x62 bit 7 polled | 0x62 bit 7 = power-down complete [C]; 0x60/0x61 masks [A] |
| 0x66 | 00 | low bits `0` | reference/voice source select for line out 2 (5 only for `xref`) [E] |
| 0x70–0x7c | `03 c0 51 02 03 1f 2f 16 d0 e0 ff 00 00` | Mikey | headset detect, mic bias, buttons: AppleCS42L63Mikey's register *n* is 0x70 + *n* (`0x80c7f480`/`0x80c7f490`/`0x80c7f4a0`) [C] |
| 0x7d | 00 | `a5` right after reset and at the end of power-down, `00` at init | ? [E] — keep iOS's order |
| 0x7e, 0x7f | | `83`, `f0` under the 0x99 key, at every init | hidden trims [E] |

**Analog volume scale** [C] (`0x80c7e8b8`, IOAudio2 ranges): v = 0..13 →
−76..−50 dB in 2 dB steps, v = 13..62 → −50..−1 dB in 1 dB steps; register
= `(v + 0x41) & 0x7f` (v = 0 → `41`, 62 → `7f`); `40` = mute. The same as
the CS42L73's HP/LO analog volume (`hpaloa_tlv`, offset `0x41`). The
speaker's range is reported shifted by `spkr-gain-db` (6 dB, 32.32 fixed
point); that changes no register.

**Mixers** [E] (which register iOS writes for which path is [E]; the source
names and the bit-6 reading are [A]): four groups of eight 6-bit
attenuations (`00` = 0 dB, 1 dB/step, `3f` = mute; bit 6 in the second half
= take the source's right channel, the reset value there is `7f`):

| Group | Output | +1 / +5 | +2 / +6 | +0, +3 / +4, +7 |
|---|---|---|---|---|
| 0x3c | headphones (HP A/B) | **ASP L → A / ASP R → B** | voice | mic paths (sidetone) |
| 0x44 | line out 2 = speaker | **ASP L → A / ASP R → B** | voice | mic paths |
| 0x4c | ? (XSP/VSP out) | | | |
| 0x54 | ASP out (recording) | | | 0x54 = DMIC B, 0x57 = MIC2 |

The master volume goes into the active group's +1 and +5 (`(0x3e − v) & 0x3f`,
+5 with bit 6); inactive entries get `3f`/`7f`.

### Sequences (exact, from `k93-cs42l63-sequences.py`)

`W r=v` write, `U r/m=v` masked update, `R r` read; GPIO levels are pin levels.

**Cold init** (`init_cold`, `initHardware(0)`):

    MCLK on (I2S0 must be putting 6.000 MHz on the codec's MCLK)  [the codec asks for it first]
    codec_reset LOW; sleep 10; codec_reset HIGH; sleep 1
    W 7d=a5; sleep 10; W 7d=00; R 05
    W 00=99; W 7e=83; W 7f=f0; W 00=00
    U 0a/8f=00; W 0c=11; W 0e=11; W 10=91; U 0f/10=00; W 1c=82; W 27=0c; U 09/07=00
    U 75/c0=80; W 73=c0; U 71/21=20; W 76=8f            (Mikey: detect off, mic bias config)
    U 06/01=00                                           (power up)
    W 12=c9
    -- routing with nothing playing (setOutputActive(0)):
    spkr_mute LOW; W 1d=19; W 3d=3f 3e=3f 41=7f 42=3f 45=3f 46=3f 49=7f 4a=3f
    U 3b/07=03; U 66/03=00; U 08/ac=ac; W 23=40 25=40 21=40 22=40 1f=19 20=19; U 07/7c=7c
    -- mic path idle (setInputActive(0)):
    U 14/07=00; W 16=00 17=00 18=00 19=a0 1a=a0 1b=a0 5e=00; U 14/10=00; U 15/e0=00
    U 06/1e=1e; W 5f=88; W 3c=3f 3f=3f 40=7f 43=3f 44=3f 47=3f 48=7f 4b=3f 4c=3f 4f=3f
    W 50=7f 53=3f 54=3f 57=3f 58=7f 5b=3f; U 0d/80=80; U 08/50=50
    -- routing once more (route), identical to the first; then the Mikey's detection starts

**Wake** (`init_wake`, `initHardware(1)`): no reset and no `7d=a5`/sleep:
`W 7d=00`, then as above (the Mikey part differs: `U 74/18=08; U 75/40=40;
R 7b; R 7c; U 71/21=21; …`).

**Sample rate** (`rate_*`): `U 11/0f=09` (48 kHz), `=08` (44.1 kHz).

**Start playback on the speaker** (`speaker_start`; speaker unmuted, volumes
0x3e):

    W 1d=19; W 3d=3f 3e=3f 41=7f 42=3f; W 45=00 46=3f 49=40 4a=3f
    U 3b/07=01; U 66/0f=00; U 08/ac=2c
    W 23=40 25=40                     (HP muted)
    W 21=7f 22=7f                     (speaker analog volume, v=62)
    W 1f=00 20=00                     (speaker digital 0 dB)
    U 07/7c=64                        (power line out 2)
    spkr_mute HIGH                    (amp on, last)
    sleep 50                          (only when the stream starts with the speaker unmuted)

**Start playback on the headphones** (`hp_start`):

    spkr_mute LOW                     (first)
    W 1d=00; W 3d=00 3e=3f 41=40 42=3f; W 45=3f 46=3f 49=7f 4a=3f
    U 3b/07=00; U 66/03=00; U 08/ac=2c
    W 23=7f 25=7f                     (HP volume v=62, or 40 for a muted side)
    W 21=40 22=40; W 1f=19 20=19      (speaker off)
    U 07/7c=1c                        (power HP A/B)

**Stop** (`speaker_stop`, `hp_stop`, identical): `spkr_mute LOW; W 1d=19;
W 3d=3f 3e=3f 41=7f 42=3f 45=3f 46=3f 49=7f 4a=3f; U 3b/07=03; U 66/03=00;
U 08/ac=ac; W 23=40 25=40 21=40 22=40 1f=19 20=19; U 07/7c=7c`.

**Switching** while playing: each control change is a full routing pass
(`speaker_mute_while_playing`, `hp_left_unmute_while_playing`, …). Going to
the headphones: speaker mute → GPIO 39 low first, `07`=`7c`; then each HP
side → `07`=`3c` then `1c`. Going back: HP mutes → `07`=`5c`/`7c`; speaker
unmute → `07`=`64`, GPIO 39 high last. Both on at once (not an iOS state):
`07`=`04`, `3b`=`00`. (`07` values here are the field under mask `7c`;
the register reads them with bits 1:0 = `11` from reset, e.g. `67`.)

**Volume**: speaker (`0x139`) `W 21=W 22=(v+0x41)&0x7f; W 1f=W 20=00`;
HP left (`0x132`) `W 23=…`, right (`0x134`) `W 25=…`; master (`0x131`) =
a routing pass with `45=(0x3e−v)&0x3f, 49=0x40|…` (speaker) or
`3d`/`41` (HP).

**Mute**: iOS mutes an output by powering it down through the routing
(above); with `dmut` (0x130) the mixer gets `3f` and stays powered. For a
driver the cheap equivalents are: speaker GPIO 39 low + `21`/`22`=`40`;
HP `23`/`25`=`40`.

**Power-down** (`power_down`, `powerDown(1, 0)`):

    U 12/c0=00; W 23=40 25=40 21=40 22=40; W 1d=19 1f=19 20=19; W 07=7f
    (Mikey: detection off — W 70, U 71, U 74, W 79/7a, reads of 7b/7c, 10 + 35 ms; depends on the jack state)
    U 75/80=00
    U 06/01=01; poll R 62 until bit 7 (sleep 10 between reads)
    U 06/40=40; sleep 10; W 7d=a5

iOS never turns MCLK off for the codec once it asked for it (vt+0x460 is
only ever called with 1) and does not assert reset at power-down [C].

**Mics** (cheap, [E]): internal (DMIC B) `U 14/07=00; W 16=17=18=00;
W 19=1a=1b=a0; W 5e=20; sleep 20; U 14/10=10; U 15/e0=c0; U 06/1e=0e;
W 5f=88; sleep 50; mixer 0x54=00 (others 3f); U 0d/80=80; U 08/50=10`.
Headset (MIC2): `W 16=17=18=34; W 5e=00; U 15/e0=20; U 06/1e=16; 0x57=00`.
Gain mapping (`0x80c7e540`, its divider `0x80c7f930`): section 6.

### Delays and pop handling

- Reset: low ≥ 10 ms, then 1 ms; `7d=a5` + 10 ms after reset; 50 ms after
  starting a stream on the speaker; power-down: poll 0x62 bit 7 every 10 ms,
  then 10 ms.
- `headphone-pop-workaround = 2` is the Mikey's (`0x8091c170` reads it,
  boot-arg `hp-pop-workaround` overrides): bit 1 means, when the mic bias
  comes on (`0x8091c63c`), set the TS3A8235 to state `0x103` (slow ramp), write
  Mikey reg 0x74, sleep the switch's `slow-ramp-delay` (50 ms) and restore the
  switch state -- skipped while the switch is in a slow state already, as it
  is while iOS measures the plug (section 6). It concerns jack insertion, not
  the playback path.
- iOS does no software ramping (no stepped volume writes); its anti-pop
  measure is the order: speaker amp enabled last / disabled first, outputs
  powered after their volumes are set and before they are muted. Whether
  `1c`=`82` or `27`=`0c` turn on the chip's own soft-ramp/zero-cross is [?]
  (datasheet).

### TS3A8235 headset switch (I2C0 0x3b) [E]

AppleTS3A8235HeadsetSwitch setState (`0x80928d24`, encoder `0x80928bfc`):
with K93's empty `switch-config` it writes only **reg 2**: state 1 → `42`,
2 → `43`, 3 → `46`; with the slow-ramp bit `0x100`: `02`, `03`, `06`
(`0x203` = `46`). The detection (AppleCSCodecMikey, `0x8091bc00`) tries
states 1 and 2 (| `0x100`) and settles on one (section 6): state 2 only if
state 1 reads reverse-bias or short and state 2 does not, so plain 3-pole
headphones (short in both) stay in state 1 [C].

## 2. I2S0 for the codec

The audio0 template (`reg` = `11000000 02000100 808d5b00 …`) decoded
[C] (`0x80df4a94`, AppleARMPlatform `0x804acf28`): `[0]` = `11`: I2S0 is
**clock master** (bit 0) and **must supply MCLK** (bit 4); format 0
(I2S); clock mode 0 (MCLK fixed by the template); **2 slots, slot width =
sample width**; **MCLK = 6 000 000 Hz**; TX/RX slot masks 3; 2/2 channels;
**16/16 bits**; rate filled in at run time (`[0x10]/[0x12]` = num/den, Hz =
num·1000/den). So: 16-bit stereo, 32 BCLK per frame, 6 MHz MCLK.

AppleSamsungI2SController (vtable `0x80df60c8`), [E] with nclk = 24 MHz:

| Step | Writes |
|---|---|
| codec asks for MCLK while idle: setMCLKEnable(1) `0x80df5534` → setPower(0) `0x80df4968` | PMGR I2S0 on, AE2 gate on (to reach the registers); TXCOM=0, RXCOM=0, **CLKCON=`0x1b4`**, wait for CLKCON bit 1; switch route(0, `0x20`); AE2 gate off; PMGR I2S0 off |
| stream: setPower(1) | PMGR I2S0 on (clock-gates 115), AE2 gate on (`advW` 0) |
| configure `0x80df4d78` | switch route(3, `0x22`); **STATUS=1, TXCON=`0x03000081`, RXCON=`0x1000`, TXCOM=0, RXCOM=0, CLKCON=`0x181`** |
| start(TX) `0x80df5408` | **NCO 0 = 1 536 000 Hz (48 kHz) / 1 411 200 Hz (44.1 kHz)**, then **TXCOM=`0x01feca06`** |
| start(RX) | RXCOM=`0x01feca06` |
| stop `0x80df5460` | TXCOM=0 / RXCOM=0, DMA stopped |
| setPower(0) after the stream (MCLK still wanted) | TXCOM=0, RXCOM=0, CLKCON=`0x1b4`, route(0, `0x20`), NCO 0 = 0, gates off |

The register values are the same at 44.1 and 48 kHz; only the NCO changes
(NCO = 32·fs = BCLK, TXCON bit 7 = BCLK is the NCO undivided). 16-bit DMA:
dma-channels entries 0 (TX) and 1 (RX). "Format 0 = I2S" is [A] (the
template's format bits only reach TXCON bits 11/12/15 and RXCON bit 9, all 0
here, the same as the mini's I2S1, which runs plain I2S).

**MCLK comes from "nclk", not the NCO** [C]: in clock mode 0 iOS compares
the template's MCLK with the provider's `nclk` clock and sets CLKCON bits
8:7 = 1/2/3 for MCLK = nclk, nclk/2, nclk/4 (configure `0x80df5120`,
setMCLKEnable `0x80df5534`); without a match the format is refused
(`0x80df4c66`). `nclk` is AppleS5L8940XIO's named clock = 
`gPEClockFrequencyInfo.fix_frequency_hz` (`0x809ce61a`), the ADT
`fixed-frequency` iBoot fills in — 24 MHz on the A5 [A] (the DWI driver
divides it the same way, `docs/research/p105-backlight.md`), hence bits
8:7 = 3 (`0x180`) and **MCLK = 24 MHz / 4 = 6 MHz** in both the idle
(`0x1b4`) and running (`0x181`) values. With `--nclk` the emulator shows the
alternatives (12 MHz → `0x101`/`0x134`, 6 MHz → `0x81`/`0xb4`). [?] Confirm
on the device: with CLKCON = `0x1b4` and switch `+0x30` = 1, pin 0's MCK
pad toggles (sample it like the mini's `audmmio` survey; a 6 MHz clock looks
random to a slow sampler, a stopped one does not), and the codec leaves
power-down (an L73-family part completes PDN=0 only with MCLK [A]).

CLKCON bits as iOS uses them: bit 0 run; bits 8:7 MCK from nclk (0 = MCK is
the NCO — the mini's I2S1 case); `0x34` (bits 2, 4, 5) stopped; bit 1 reads
1 when stopped.

## 3. Port plumbing for i2s0

From the ADT (`k93-adt-audio.py`) and the drivers:

| Item | Value |
|---|---|
| Registers | `0x34190000`, `i2s-version 3`, AIC IRQ **99** (`0x63`) |
| PMGR | I2S0 power state **`0x3f1010a0`** (clock-gates 115); AUDIO `0x3f10109c`; AUDIO-CLK `0x3f100058`; CDMA `0x3f101078`; NCO_REF0 clock `0x3f1000c0` |
| AE2 index | **0** (`function-pclk_gate` = ae2 `advW` 0): ACS `0x341a0000 + 0x10` = 0 to ungate, 1 to gate (AppleA5AE2 setDeviceClock `0x804f4ee0`, same code as the mini's) |
| NCO | **0** (`function-mclk_frequency` = pmgr `NCOf` 0): PMGR `+0x110`, recipe of `setNCOFrequency` `0x809d0280` (identical to the mini's: CTRL=`0x90000000`, +4 = 2·Hz, +8 = 2·Hz − Fref, CTRL=`0x90000400`, `0x90000c00`) |
| Fref | the pmgr node's clock 0 (clock-ids `0x13c`); 342 MHz on the mini [A] — [?] verify on K93 (read NCO_REF0 / PLL3, or time the FIFO drain as on the mini) |
| CDMA TX | channel **0x1f**, request line **1**, FIFO **`0x34190010`** (DAR `0x190010`), transfer `0x00040002` (16-bit, burst 4) / `0x00040004` (32-bit) |
| CDMA RX | channel **0x20**, request line **0**, FIFO **`0x34190038`** |
| Pads (pin group 0) | not in the ADT or any kext (iBoot sets them). The GPIO block is the same `gpio,s5l8940x` as the mini's, where pin group 0 is MCK 76, BCLK/LRCK/DOUT 77–79, DIN 80 [A]; [?] confirm by sampling the input bits of 70–100 while I2S0 runs |

**i2s-switch** (`0x3fa01000`, AppleAE2I2SSwitch1 vtable `0x80ce8498`, 16
registers, nodes `hp0 hp1 hp2 hclk ap0 ap1 ap2 ap3 pin0..pin3`) [E]: the ADT
default route `hclk → hp0` writes nothing; i2s0's route `ap0 → pin0`
(`03 03 33`):

| iOS call | Registers |
|---|---|
| MCLK only (idle): route(0, `0x20`) | **`+0x30` (reg 12) = 1** — MCK on pin 0 |
| stream, TX+RX, MCLK: route(3, `0x22`) | **`+0x00` = `0x44`**, `+0x30` = 1 |
| TX only: route(2, `0x22`) | `+0x00` = `0x44`, `+0x30` = 1 |
| RX only: route(1, `0x22`) | `+0x00` = `0x04`, `+0x30` = 1 |
| I2S0 as slave (not used) | `+0x00` = `0x40` |
| off: route(0, 0) | all 0 |

So reg[pin] bit 2 = pin's frame clocks driven by apN, bit 6 = apN's TX data
on the pin, reg[12 + pin] bit 0 = MCK on the pin. The router refuses
cross routes (ap1 → pin0 and ap0 → pin1 return `0xe00002e6`); i2s3's
`ap3 → pin3` gives reg 3 = `0x44` the same way.

**GPIO polarities and order** [C] (AppleD1946PMU `_setGPIOFunction`
`0x80e9ab00`, AppleS5L8930X GPIO function `0x80debaa8`): both
`function-*` GPIOs carry flags `01 00 01 00` = output, **active-high 0
(so the pin level is the inverse of the value iOS passes)**, initial value 1.

- **Codec reset, PMU GPIO 13** (PMU reg `0x61 + 13 = 0x6e`, bit 1 =
  level): driven **low as soon as AppleEmbeddedAudio looks the function up**
  (start), held through initFromProvider; `initHardware(0)`: MCLK on first,
  then value 1 (low) → 10 ms → value 0 (**high = running**) → 1 ms → I2C.
  Active low. DT: `reset-gpios = <&pmu 13 GPIO_ACTIVE_LOW>`.
- **Speaker amp, SoC GPIO 39**: low at creation (= muted); the routing
  drives it low **before** any register change when the speaker is to be off
  and high **after** the last write (0x07) when it is to be on, then 50 ms on
  a stream start. Low = muted, high = playing: an active-high enable
  (`enable-gpios = <&gpio 39 GPIO_ACTIVE_HIGH>`).

## 4. Machine-readable sequences

`tools/audio/k93-cs42l63-sequences.py` (generated; `python3
tools/audio/k93-cs42l63-sequences.py` prints JSON): 33 entries — `probe`,
`init_cold`, `init_wake`, `rate_48000`, `rate_44100`, `speaker_start`,
`speaker_stop`, `hp_start`, `hp_stop`, `speaker_volume_*`,
`master_volume_*`, `hp_left_volume_*`, `hp_right_volume_*`,
`speaker_mute_while_playing`, `hp_*_unmute_while_playing`,
`hp_*_mute_while_playing`, `speaker_unmute_while_playing`,
`route_speaker_and_hp`, `power_down`, `input_*` — each a list of
`{'op': 'write'|'update'|'read'|'sleep_ms'|'gpio'|'mclk', …}` in iOS's order;
and `GAINS`, iOS's input-volume mapping for each source (section 5).

## Suggested Linux bring-up order (from the above)

1. PMGR AUDIO-CLK (`0x80000001` on the mini), AUDIO, I2S0 power states on;
   ACS `+0x10` = 0; I2S0 CLKCON = `0x1b4`; switch `+0x30` = 1 (6 MHz on MCK).
   (iOS then gates I2S0 again while idle; keeping it on is simpler.)
2. Reset low ≥ 10 ms, high, 1 ms; `init_cold` from `W 7d=a5` on.
3. hw_params: `U 11/0f=<rate code>`; I2S0 STATUS=1, TXCON=`0x03000081`,
   RXCON=`0x1000`, TXCOM=0, RXCOM=0, CLKCON=`0x181`; switch `+0x00`=`0x44`;
   NCO 0 = 32·fs; CDMA ch 0x1f → `0x190010`, request 1, 16-bit.
4. Trigger: CDMA, TXCOM=`0x01feca06`; then `speaker_start` or `hp_start`.
5. Stop: `speaker_stop`, TXCOM=0, CDMA off, CLKCON back to `0x1b4`.
6. Suspend: `power_down`; MCLK may then go (CLKCON=`0x34`, switch `+0x30`=0).

## 5. On the iPad (2026-11-10): capture, the input gain, the jack

**Capture works as iOS's data says**: CDMA channel 0x20, request line 0,
I2S0's RX FIFO at +0x38, `DCR = 0x24` (16-bit, burst 4, device to memory:
the mini's DCR bit 1 cleared), `RXCOM = 0x01feca06`.  The switch's
capture-only route (`+0x00 = 0x04`) carries the frame clocks: the codec
answers.  A codec-internal loopback proves the whole path without a
microphone: ASP in -> ASP out, mixer `0x55 = 0x00`, `0x59 = 0x40` (group
0x54 +1/+5), records what plays.

**The internal mic recorded zeros at first -- iOS's quietest input volume.**
The extracted `input_internal_mic` ran with the input volume control
(0x16f) at the framework's first value, 0, which iOS maps to registers
0x19-0x1b = `0xa0`: the digital volume, two's complement, `0x00` = 0 dB --
-96 dB on iOS's scale (section 6).  iOS's mapping for the internal mic, run
for every volume (`GAINS` in `k93-cs42l63-sequences.py`): boost 0x14 bits
2:0 = 0, from v = 109 on 7; 0x16-0x18 = 0; 0x19-0x1b = `0xa0 + v`, from 109
on `v - 116` -- v - 96 dB on iOS's scale, the boost worth 20 steps.  (This
note said 0.5 dB a step here at first, from a room-noise ratio: +20 steps
raised it 2.9x.  Room noise turned out to be no measure on this codec --
section 6.)  Speech at arm's length peaks at -8 to -14 dBFS at v = 120.
iOS records the internal mic in mono on the ASP's left channel (path A,
mixer `0x54`); `0x58 = 0x00` (path A into the right channel, 0 dB) copies it
-- measured sample for sample.

**The jack** (measured with a plug going in and out): Mikey register
**0x77 bit 7 = a plug is in** (level); **0x7b** latches bit 6 on insertion and
bit 5 on removal and clears when read; 0x62/0x63 clear when read too.  The
codec's interrupt line (SoC GPIO 15) did not move: the Mikey's interrupts are
masked (`0x79 = e0`, `0x7a = ff` after iOS's init; iOS's power-down writes
`7a = fe`) -- the driver polls 0x77, twice a second.

**TS3A8235** (I2C0 0x3b): register 0 reads `ff`, register 2 is the state
(bit 6 is not stored: 0x42 reads back 0x02), register 3 a status that
follows it: state `02` -> `92`, `03` -> `61`, `06` -> `c3`, `00` -> `00`.
With a CTIA headset (the user's) and the HP path on: **untouched since
power-up the switch passes nothing** (silence); written, `0x00`, `0x42` and
`0x46` sound right and **`0x43` (iOS's state 2) leaves it nearly silent** --
the OMTP wiring, ground on the CTIA mic contact.  So state 1 (`0x42`) is
CTIA, the driver's choice when a plug goes in.

**The headset's microphone recorded exact zeros** at first, in every switch
state, at the top of the volume range used then, with the mixer's path A or
B, and with 0x06 bits 2 and 1 cleared as well: the analog input path (MIC2
-> PGA -> ADC) showed not even its noise.  The mic's bias was off: iOS's
Mikey code turns it on when it finds a mic in the plug -- section 6.

## 6. The headset's microphone (2026-11-10): the Mikey block

**How it was read.** Ghidra 12.2, headless (in the `eclipse-temurin:25-jdk`
container), on two slices of the kernelcache loaded as raw ARMv7 at their
addresses: `0x808ff000`-`0x80930000` (AppleEmbeddedAudio,
AppleCSEmbeddedAudio -- AppleCSCodecMikey's code -- and
AppleTS3A8235HeadsetSwitch) and `0x80c7d000`-`0x80c81000`
(AppleCS42L63Audio).  A pre-script finds each kext's Mach-O header in the
slice, splits its segments, seeds Thumb functions at `push {..., lr}`
prologues and at every Thumb pointer in the vtables, and names each 16-byte
stub after the pointer it jumps through.  Virtual calls were resolved by
hand from AppleCS42L63Mikey's vtable (`0x80c80628`).

**The Mikey's registers** [C]: AppleCS42L63Mikey reads, writes and updates
0x70 + n through the codec (`0x80c7f480`/`490`/`4a0`), AppleCSCodecMikey's
accessors sit at its vtable +`0x468`..+`0x4a8`.

| reg | iOS's use |
|---|---|
| 0x70 | written whole by every mode: `01`, `c1` in the button modes 6, 7, 9 |
| 0x71 | bits 7:6 by mode (`11` in all but modes 1 and 2); bits 5 and 0 `21` at wake, `20` after |
| 0x73 | `c0` at init (the other pop workaround, not K93's, toggles bit 5) |
| 0x74 | **the mode**: bits 2:1 = `01` is the mic bias off, anything else on (`0x8091c63c`) |
| 0x75 | bits 5:0 the comparators' reference: `17`, `36`, `34`, `3c` (`0x8091c43c`); bits 7:6 at init and wake |
| 0x76 | `8f` at init |
| 0x77 | bit 7: a plug is in (measured) |
| 0x78 | the comparators: bit 0 the mic contact shorted, bit 1 above the reference |
| 0x79, 0x7a | interrupt masks (power-down leaves `ff`, `fe`) |
| 0x7b, 0x7c | interrupt status, read-clear: 0x7b bits 6/5 insertion/removal (measured), the rest Apple's remote's data and the button's events (`0x8091c758`) |

**iOS's modes** (`0x8091c464`) as K93 runs them (its `0x4ac` returns 0,
`headphone-pop-workaround` = 2) [C]:

| mode | used for | 0x70 | 0x71 7:6 | 0x74 | then |
|---|---|---|---|---|---|
| 0, 3 | Detached, HeadphonesOnly; between measurements | `01` | `11` | `03` (bias off) | 10 ms |
| 1 | -- | `01` | `10` | `03` | 10 ms |
| 2 | power-down | `01` | `00` | `0d` | 35 ms |
| 4 | measuring the plug | `01` | `11` | `06` | 35 ms |
| 5 | SingleButton's check | `01` | `11` | `1c` | 35 ms |
| 6, 7 | HeadphonesWaiting, SingleButton | `c1` (after 0x74) | `11` | `1f` | 35 ms |
| 8 | **MicOpen, WaitingForMic** | `01` | `11` | **`1e`** | 35 ms |
| 9 | Mikey (Apple's remote) | `c1` (after 0x74) | `11` | `5f` | 35 ms |
| 10 | waiting for a plug | `01` | `11` | `9e` | 35 ms |

**The detection** (`0x8091bc00`, `0x8091bd14`), when a plug goes in: mode 3;
then for the switch's states 1 and 2 with the slow-ramp flag (`02`, `03`):
reference `17`, mode 4, the slow-ramp delay (50 ms), 0x78: bit 0 *short*,
bit 1 *reverse-bias*, else reference `36`, 1 ms, bit 1 *mic*, else *open*;
mode 3, 50 ms.  The wiring kept: state 2 if state 1 read reverse-bias or
short and state 2 open or mic, else state 1 (`0x401` if both look alike).
The states are AppleMikeyReceiver's: Detached, HeadphonesOnly,
HeadphonesWaiting, SingleButton, MicOpen, WaitingForMic, Mikey.  A plug with
a mic and no Apple remote ends in MicOpen (reference `3c`, mode 8) or
WaitingForMic (`36`, mode 8): the bias on either way (`0x8091b2e4`,
`0x8091af98`).

**The pop guard** (`0x8091c63c`): when the bias comes on from off with the
switch in a normal state, the switch goes to state 3 slow (`06`) while 0x74
is written, for the slow-ramp delay, then back.

**On the iPad**, a CTIA headset (the user's) plugged in, recording MIC2
while 0x74/0x75 were stepped through iOS's modes by hand: bias off (`03`),
exact zeros; MicOpen (`3c`, `1e`), SingleButton (`36`, `1f`, 0x70 `c1`) and
mode 4 (`17`, `06`), the voice -- 0x78 reads `d2` in MicOpen.  Both wirings
measured, that headset reads "mic" in CTIA and "reverse-bias" in OMTP, as
iOS's logic has it; the driver measures OMTP only when CTIA reads
reverse-bias or short, the only case where iOS's choice depends on it.

**The input volume, again.**  The voice came ~50 dB too quiet at first: the
table ran iOS's mapping for v = 0..160 for both mics, but iOS's input ranges
(getControlRange `0x80c7e94c`; `0x807f207c` builds each as `start int
value`, `start db value`, `integer steps`, `db per step`) differ by source
[C]:

| source | v | iOS's dB | iOS's default |
|---|---|---|---|
| internal (DMIC B) | 0..128 | -96 + v | 128 = +32 dB |
| headset (MIC2) | 0..312 | -92 + v / 2 | 248 = +32 dB |
| line in (not on K93) | 0..292 | -99 + v / 2 | 198 |

The headset's mapping (`0x80c7e540`) [C]: u = v - 204; above 19, u = v - 224
and the analog stage's bit 7 (+10 dB on that scale); the analog stage takes u
clamped to -12..+24 (bits 5:0, 0.5 dB steps: `34` = -6 dB), the boost (+20
dB) what is above 24, the digital volume half of the rest (`0x80c7f930`
divides).  So below v = 192 only the digital volume moves, from 192 up only
the analog stage; iOS's default is the analog stage at +12 dB with bit 7,
the digital volume at 0 dB.  The driver's "Capture Volume" puts both mics on
iOS's dB scale: i = 0..128 = -96..+32 dB, v = i for the internal mic and 2i
- 8 for the headset (`GAIN_V` in `k93-cs42l63-emu.py`), default 120 = +24 dB.

Measured with speech on the headset: at 120 the voice close to the mic peaks
at -9 dBFS (words about -25 dBFS RMS, the room between them -65 dBFS); 108,
twelve steps down (the analog stage `88` -> `04`), takes 12.8 dB off -- iOS's
scale within a dB.  Room noise is no measure of the steps on this codec: on
the internal mic, interleaved every 0.7 s (medians of 8), +20 digital steps
raised it 6.6 dB, +40 steps 10.8 dB, the boost 13.7 dB -- no single step size
fits.  Something shapes levels near -80 dBFS (open point 7).

**Clicks, heard with earbuds in** (the user's, a CTIA headset going in and
out, each step on its own):

| step | heard |
|---|---|
| the switch alone, `00` -> `42` (or via `02`); 0x70/0x71's mode writes | (almost) nothing |
| the bias coming on bare, or behind a `46` guard | a soft click |
| the bias coming on behind iOS's `06` guard | nothing |
| the bias going off bare, or behind a `46` guard | nothing |
| the bias going off behind a `06` guard | a soft click |
| a plug pulled out with the bias on | a click, every time |
| iOS's whole detection (both wirings, the bias cycled in the slow states) | clicks |

So iOS's own rule -- the guard (`06`) when the bias comes on, none when it
goes off -- is the silent one, and the switch seems to move gently into its
states with bit 6 set and abruptly into the others.  The driver measures in
the normal states with that rule, and keeps the bias on only while the
headset's mic records (iOS keeps it on as long as the plug is in, for the
button): plugging in, recording's start and stop and pulling out are all
silent.

**Not done:** Apple's remote (Mikey's own protocol, mode 9) and a headset's
button (0x7c, the button modes; it needs the bias on all the time, which
clicks on unplugging), WaitingForMic as a state of its own, the codec's
interrupt (the driver polls 0x77).

## Open points and the experiments that settle them

1. **nclk / MCLK = 6 MHz** — strongly constrained (iOS refuses the format
   otherwise) but not measured: check MCK on pin 0 or that the codec leaves
   power-down.
2. **NCO Fref on K93** — read PMGR NCO_REF0 (`0x3f1000c0`) and PLL3; or run
   I2S0 with CLKCON = `0x181` and time the TX FIFO drain at a nominal 48 kHz.
3. **Pads of pin group 0** — sample GPIO 70–100 input bits with I2S0 running.
4. ~~**TS3A8235 state for plain headphones**~~ — settled (section 5): `42`
   for a CTIA headset; `46` and `00` (written) also carry the sound.
5. **Register names** marked [A]/[?] (0x06 bit 6, 0x09, 0x0a, 0x1c, 0x27,
   0x66, 0x7d, mixer group 0x4c) need the CS42L63 datasheet; the sequences do
   not depend on them.
6. **iOS 6.1.3** (on the iPad) vs 8.4.1 (analysed) — a dump of the codec
   registers from iOS 6 while playing to the speaker would cross-check the
   playback state (expected from 8.4.1: `06`=`1e`, `07`=`67`, `08`=`7c`,
   `11`=`08`/`09`, `1d`=`19`, `1f`=`20`=`00`, `21`=`22`=volume, `3b`=`01`,
   `45`=`00`, `49`=`40` at full master volume).
7. **The input steps' true size** — iOS's scale (1 dB a digital step, the
   boost 20 dB) is kept; room-noise ratios do not fit any step size (section
   6).  A steady tone from the speaker into the internal mic, recorded at
   several digital volumes and with the boost, settles it.
8. **A headset's button and Apple's remote** — the Mikey's interrupt status
   (0x7b, 0x7c) while a button is pressed, in modes 7 and 9; the codec's
   interrupt line (SoC GPIO 15) with 0x79/0x7a unmasked.
