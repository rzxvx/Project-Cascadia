# iPad 8 internal speakers

This is the contributed **iPad 8 Wi-Fi, J171aAP / iPad11,6 / T8020** speaker
port. It is not an A12-wide audio implementation. XR and XS Max receive no
speaker nodes, GPIO mappings, m1n1 audio overlay or rootfs audio policy.
The common kernel can contain the driver; its DT match and machine check
are what permit it to bind.

## Status and limits

On the experimental iPad build we verified internal-speaker playback, touch
while playing, desktop/browser audio, both volume buttons and Omarchy's
volume indicator. The v0.3 Omarchy userspace was tested with the existing
Cascadia kernel/boot flow and Pixman renderer, not Orchard's A10 kernel.

The normal kernel, native m1n1, DTB, selected-root installer and packaging
entry points completed a host build and booted the iPad to the v0.3 greeter.
Startup attenuation2, speaker gate, ALSA card, touch/key devices and radio
loading were checked; after login the user reported "all working". A fully
clean-checkout reproduction and separately instrumented replug/stress checks
remain pending. Do not confuse successful experimental boots with testing
all automated entry points. XR/XS behavior is isolated by design; they were
not reboot-tested as part of this audio integration.

Supported playback is **48 kHz, four-channel, S16_LE**, through SmartIO DMA.
There is no microphone, headphone, jack-detection or iOS speaker-protection
implementation here. Recording monitors are not microphones. Native iOS
uses additional signal processing; audible output is not proof of identical
frequency response, acoustic power or protection.

## Sources and credits

* [Asahi Linux](https://github.com/AsahiLinux/linux): the RTKit/SmartIO,
  DMAengine, clock and power-management foundations. Existing source
  licensing and authorship are retained.
* [HoolockLinux kernel](https://github.com/HoolockLinux/linux): the pinned
  Cascadia A12 kernel baseline, `6831bc701a6ce059e71e5aaa9488c9195bea6927`.
* [Orchard's S8000/T8010 MCA driver](https://github.com/OrchardLinux/linux/blob/main/sound/soc/apple/s8000-mca.c)
  and [T8010 TDM correction](https://github.com/OrchardLinux/linux/commit/2c2052da799b):
  Berkay Kösebay and Orchard contributors' audio work informed the MCA,
  NCO, four-channel TDM and conservative sample-attenuation approach.
  iPad 7 inherits Orchard's iPad 6 speaker description. Their `/4` is a
  Linux ceiling, not a recovered iOS gain constant.
* [Cascadia A5 audio](../a5/docs/research/p105-audio.md) was a comparison,
  not our amplifier driver. A5 uses CS35L19 with a programmable hardware
  volume register. Its gain-register programming cannot be copied to our
  MAX98357 path.
* [Orchard Omarchy A v0.3](https://github.com/OrchardLinux/omarchy-a/releases/tag/v0.3.0)
  and [Omarchy](https://github.com/omacom/omarchy): desktop, mixer, OSD,
  keyboard, application and power-policy code. We adapt the iPad-specific
  integration rather than claiming those applications as our work.
* Device-specific work in this port: decode the **iPad's own** Apple device
  tree, kernel and live read-only PCM evidence; correct the J171 handoff,
  SIO power lifetime, MCA routing and NCO match; implement the PCM endpoint,
  native channel duplication and board-specific input/desktop glue.

Native reference: **iPadOS 17.1.1, build 21B91**, the iPad11,6/7 kernelcache
and J171a device tree. This is not an XR or iPad 7 kernel. We used local IDA
analysis and bounded local disassembly/cross-references. Apple firmware,
SysCfg, native memory dumps and device credentials are not distributed here.
Consumers do not need an iOS jailbreak to use the resulting Linux driver.

## Hardware and data flow

```
application -> PipeWire stereo sink -> L,L,R,R routing
  -> four-channel ALSA PCM -> sample attenuation
  -> SIO endpoint 0x2c -> MCA1 transmit/serial adapter
  -> MAX98357 amplifier bank -> internal speakers
```

The J171 description uses eight 16-bit TDM slots, four carrying samples,
with a 6.144 MHz bitclock at 48 kHz. GPIO6 enables the amplifier bank; it
starts LOW and is not enabled until the playback trigger owns the clocks.
The NCO's input-zero parent is **900 MHz**, obtained from the live J171
clock table, not borrowed from a different SoC's oscillator frequency.
The T8020-compatible NCO path uses its native control/enable encoding;
generic NCO matches retain their separate behavior.

The MCA serial adapter's native S16 packing was traced rather than inferred
from a successful DMA drain. Effective adapter value `0x5050` is consistent
with the native FIFO/config producer. Missing high write bits were not proof
of lost sample gain. A PCM drain alone never established speaker output.

## The SIO problems that had to be fixed

### Firmware handoff layout

The existing handoff's five 32-byte DMA-shim records did not match this
J171 AppleSmartIO2 ABI. Its handoff needs **six 36-byte records**, including
SSPI, SUAR, SAUD, SQSP, ADMA and AAUD, and the native device-type ordering.
The extra power-gate word is initialized to the native absent value, `-1`.
Unsupported shim-power-gating metadata is rejected rather than guessed.

The m1n1 adjustment is under `m1n1/ipad8/`, with an explicit J171a ADT
identity guard. Other boards do not take this layout. A boot that reaches
the UI without crashing is insufficient validation: the audio slot still
needs to describe the correct shim.

### Power-domain lifetime and touch interaction

Mapping the audio shim without powering it exposed a firmware fault during
interrupt handling. Holding only the audio domain then exposed the analogous
SPI-side issue. SmartIO's interrupt path can inspect other **mapped** shims;
it does not only access the endpoint that raised the interrupt.

For this board's audio-enabled SIO compatible, the driver acquires and holds
`sio-cpu`, `spi-shim`, `uart-shim` and `audio-shim` **before RTKit starts**.
Devres unwinds the holds. This is not a global power-domain override, not
an audio-only workaround and not an XR SPI-driver replacement. The existing
J171 SPI/touch branch remains in place.

### Audio DMA parameters

J171 MCA TX endpoint `0x2c` needs the native transfer/config word `0x21`,
FIFO/limit 384 bytes and trigger 204. Generic bus-width encoding discarded
necessary shim bits. The endpoint override requires the J171 machine and
its audio-specific SIO compatible. Routine audio register/config logging
uses `dev_dbg`; it is off by default.

## Channel mapping: what was actually established

Early single-channel listening was inconsistent: one slot was reported as
both grilles. We did not treat those observations as proof of a universal
physical amplifier-slot map. Initially routing only channels1 and3 also did
not reproduce native iOS's transmitted PCM.

We built a small native audio player and captured a bounded block from the
**actual iOS speaker DMA buffer**, following the DMA event source, SIO
channel state, IODMACommand and memory descriptor. Completed commands could
retire between reads; sampling an old object was not a valid PCM capture.
No MMIO writes or native gain changes were used.

With known stereo tones, the captured native buffer had:

| ALSA/TDM channel | Contents | Linux paired listening result |
| --- | --- | --- |
| 1 | Left | channels1+2: left grille only |
| 2 | Left | same paired test |
| 3 | Right | channels3+4: right grille only |
| 4 | Right | same paired test |

Thus the supported desktop mapping is **L,L,R,R**, not arbitrary surround
upmix, an L+R average or an instruction to address the grilles individually.
The persistent PipeWire copy graph implements that duplication explicitly.

## Gain and volume are different controls

The PCM divisor defaults to **4**. Root may select **4, 2 or 1** at runtime;
other values, including zero, are rejected. No mmap PCM path bypasses the
sample processor. The divisor is read once per processing block.

| Divisor | Compared with `/4` |
| --- | --- |
| 4 | current conservative default |
| 2 | +6.02 dB |
| 1 | +12.04 dB |

```
sudo ipad8-audio-gain status
sudo ipad8-audio-gain 2
```

This changes the current boot. For a fresh installed root the policy file
`/etc/cascadia/ipad8-audio.conf` selects the startup divisor. The installer
preserves an existing policy; change that root-owned file explicitly to
change its future default. The fresh-install default remains4.

The native capture used 44.1 kHz stereo tones with input peak8192 and iOS
output volume reported as1.0. Native duplicated channel pairs measured
peaks4316 and6030 for different tone frequencies. Linux `/4` would produce
2048 for that input. Those are frequency-specific observations, not one
native gain scalar. Later, with `/2` and correct routing, the user reported
YouTube loudness roughly comparable to iOS. This is a listening result,
not a calibrated equivalence or a speaker-protection implementation.

Desktop 0–100% volume, its slider and volume keys remain ordinary PipeWire
controls. Software boost can clip before S16 conversion; it is not equivalent
to removing attenuation after conversion. Enabling channel mixing did not
solve the original loudness difference.

## Buttons and Omarchy integration

* Volume up arrives through the existing SMC input (`bVUP`).
* Volume down is **AP GPIO137**, active-low, Linux `KEY_VOLUMEDOWN`, with
  20ms debounce and autorepeat. The SMC driver's advertised volume-down
  capability did not mean the physical button produced an event.
* Both clean press/release pairs were captured, followed by user confirmation
  that both buttons move the Omarchy volume indicator.
* v0.3's `omarchy-volume` supplies 5%-step controls and the existing OSD.
  Its audio panel already provides the master slider and per-app mixer.
* Old saved preferences lacked the audio widget and disabled OSD. Installing
  the policy adds the widget and enables OSD, retaining other preferences.
* The power-button regression had a different cause: v0.3 powerd watched
  only an input named `gpio-keys`; our power button is on
  `Apple SMC power/lid events`. The installer adds a **J171-only** acceptance
  path while keeping one power-policy owner. Volume events are ignored by
  powerd. User confirmed power/lock behavior after the live correction.
* Platform audio hints hide the internal routing stream and the raw hardware
  entry while the stereo endpoint exists. A playback monitor is not a mic.

The Omarchy extras are optional. Debian/XFCE uses PipeWire/WirePlumber and
its own mixer/input bindings; it does not install Quickshell or A10 powerd.
Updating Omarchy userspace does **not** provide A10 GPU/video-decoder or
suspend support to A12. The tested session used Pixman, with the iPad's own
native display/backlight and existing touch/radio/USB/NFS setup preserved.
Suspend remained disabled during these tests.

## Reproduction and review

Build using the normal selected-device paths; do not run the local research
packager or copy a prebuilt research image as proof of reproducibility:

```
JOBS=8 ./cascadia a12 --device ipad8 build kernel
./cascadia a12 --device ipad8 build m1n1
./cascadia a12 --device ipad8 build dtb
```

Keep the established board-matched firmware, ticket, SysCfg and initramfs
workflow from [README.md](README.md). The audio addition does not embed a new
radio/Apple firmware payload in the initramfs. Install policy into the root
that will be handed off after boot:

```
./cascadia a12 --device ipad8 audio-root --root /path/to/rootfs
# Explicit opt-in to divisor2 on a fresh policy:
./cascadia a12 --device ipad8 audio-root --root /path/to/rootfs --attenuation 2
```

The Debian root builder calls the installer automatically only for the
selected iPad profile. For an existing Omarchy root, run `audio-root` before
boot; it must already have v0.3's volume helper and desktop. `--device xr` or
`xsmax` makes this installer a no-op. It does not reboot, install a new OS,
format storage, change passwords, alter NFS exports or restart a live desktop.

Offline integration checks:

```
python3 devices/a12/tests/test-audio-integration.py
```

For a further clean-checkout reproduction, build through the normal scripts,
boot the iPad with capture, verify no SIO faults, then test
paired left/right tones, touch while playing, browser playback, both volume
keys/OSD, lock/wake and USB replug. Use a known input as well as listening;
a fence/PCM drain or an idle kernel log is not an acoustic test. Retain a
working bootset until this integrated path passes.

## Source map

* `linux/extras/apple-j171-speakers.c`: J171 ALSA/MCA endpoint and live divisor.
* `linux/extras/apple-sio-core.c`: gated audio endpoint and power lifetime.
* `linux/patches/linux-0027-j171-nco.patch`: board-specific NCO control path.
* `m1n1/ipad8/0013-j171-sio-audio-layout.patch`: native shim handoff ABI.
* `dts/t8020-j171a.dts`: audio resources and discrete volume-down key.
* `rootfs/install-ipad-audio.py`, `rootfs/audio-overlay/`: selected-root policy
  and optional Omarchy integration. No diagnostic playback runs at boot.
