# K93 cameras — the iPad 2's ISP, its firmware and its IOMMU

**State (2026-11-10): both cameras work as V4L2 devices** -- the ISP's CPU
runs iOS 8.4.1's firmware, takes iOS's commands and streams YUYV frames
into Linux's buffers: `drivers/media/platform/apple/apple-h4isp.c`, with
`/dev/video0` the rear camera and `/dev/video1` the front one, used by mpv
and ffmpeg (milestone 4).  It began as `tools/camera/k93isp.c` from user
space (milestones 1-3, sections "On the iPad").  Everything else read from
the iOS 8.4.1 (12H321) K93 kernelcache and root filesystem.  The plan is at
the end.

Legend as in [k93-audio.md](k93-audio.md): **[C]** read in the code, **[A]**
by analogy, **[?]** open.

## The hardware (K93 ADT)

| Block | ADT | Address (CPU) | IRQ | Clocks / power |
|---|---|---|---|---|
| ISP | `isp,s5l8940x` | `0x3a000000`, 1 MB | 7, 8 | clock-ids 298; clock-gates 110, 111; power-gates 110; reset: PMGR `ARST` 0x12 |
| its IOMMU | `dart-rt` (`dart`, mapper `mapper-isp`) | `0x3a400000`, 8 KB | 117 | clock-gates 109; `invalid-translation-target` = `0x30000000` |
| sensors | in the ISP node: `camera-rear`, `camera-front`, `sensor-type` 6 | — | — | `function-power_enable` (PMU `pmuL` 0x0a 0x03), `function-sensor_shutdown` / `_shutdown2` (GPIO bank 0x18, pins 0 and 2), `function-sensor_extclk` / `_extclk2` (pins 1 and 3) |

The sensors hang off the ISP: it has its own I2C master
(`AppleH4CamIn::ISP_I2CRead`, `SMBSYS_REG_SMSTA`), its own CPU and its own
MIPI receivers.  iOS's tuning file (`MediaToolbox.framework/K93/CameraSetup.plist`)
names them by ID: **rear `0x9726`, front `0x7739`** (OmniVision OV9726 and
OV7739 by the IDs [A]; 0.7 MP and VGA, as the iPad 2's cameras are).

Other ADT blocks around it, not needed for capture: `scaler0`/`scaler1`,
`jpeg`, the video encoder/decoder (`vxe`, `vxd`) and `dart-nrt` for them.

## The software iOS runs (iOS 8.4.1)

| Piece | Where | What |
|---|---|---|
| `AppleH4CamIn` (`com.apple.driver.AppleH4CameraInterface`) | kernelcache `0x807dc000`, 56 KB | powers the ISP and the sensors, loads the ISP CPU's firmware, talks to it through IPC channels in shared memory |
| `AppleS5L8930XDART` + `IODARTFamily` | `0x809c8000`, `0x8053a000` | the ISP's IOMMU |
| `H4ISPServices.framework` | dyld shared cache image `0x2928a000` | user space: hands the kernel the **firmware** (section `H4ISPFIRMWARE`, 1 409 028 bytes) and the sensors' **set-files** (section `H4ISPSETFILE`, 882 572 bytes) |
| `H4ISP.mediacapture` | dyld cache `0x25bc9000` | the capture pipeline on top |

The firmware is an ARM image (vector table at 0, `FFW Application`,
`H4ISP-12.50.00`, Aug 14 2014) for the ISP's own CPU; the marker iOS checks
before booting it normally, `adc` at offset `0x1040`, is there.

Getting them (all from the IPSW the K93 build already uses,
`iPad2,1_8.4.1_12H321_Restore.ipsw`): the root filesystem `058-24482-024.dmg`,
key `d33bfc10cc397ae008d2e56685acb172b74f0eccbe652ecd52e234ab12705980b6483b60`
(public, The Apple Wiki), read with `scripts/rootfs-extract.py`'s layers; the
dyld cache's mappings put `H4ISPFIRMWARE` at cache address `0x31fd9000` and
`H4ISPSETFILE` at `0x32137000`.

## Booting the ISP's CPU (`AppleH4CamIn::ISP_Init`, `0x807defc0`) [C]

Registers, offsets from the ISP's base:

| Offset | Use |
|---|---|
| `0x40004` | write 1: the ISP CPU runs |
| `0x40008` | the firmware's IOMMU address `| 0x80000000`: its boot address |
| `0x41000` | interrupt status: bits 7:4 from the ISP CPU |
| `0x41004` | interrupt enable (`0xf8` once ready) |
| `0x41020` | `0x10` after the first handshake |
| `0x41024` | interrupt clear (write `0xffffffff`) |
| `0xc3000`-`0xc301c` | eight mailbox words, both ways |

1. Mailbox and interrupts cleared; the boot address set; `0x40004` = 1.
2. Wait (up to 10 s) for `0x41000 & 0xf0`: the firmware is up.  It has
   written the number of IPC channels (`0xc3000`, at most 32) and their
   queue size (`0xc3004` + 1) and the heap it wants (`0xc300c`, at most 4 MB).
3. The driver answers in the mailbox: the firmware's base, where the IPC
   queues are, the firmware buffer's size, `0x10000000` minus it (the rest
   of the 256 MB IOMMU window), the heap's address and size, a log buffer,
   and a 0x18-byte descriptor; then `0x41020` = `0x10`.
4. Second interrupt: the **channel table** is ready, at the firmware's
   offset in `0xc3000`: 0x100-byte entries, the name at +0, type +0x40,
   source +0x44, size +0x48, queue offset +0x4c.  Channels: `SHAREDMALLOC`,
   `TERMINAL`, `BUF_H2T`, `BUF_T2H`, `IO`, `IO_T2H`, `DEBUG`.
5. `0xc300c` = `0x8042006`; the firmware clears it when it is ready.
   `0x41004` = `0xf8`.
6. Commands over `IO`: START, PRINT_ENABLE (4, 1), per camera
   CH_SET_FILE_LOAD (`0x111`: channel, set-file address, size) and a
   sensor query (`0x10d`, 0xa8 bytes back: was a sensor found), then an
   NVM override if there is one (`0xa11`).

`power_on_hardware` (`0x807dee14`) before it: the PMGR `setActive`, the
sensors' power, the device reset, the clocks; details still to read.

## The IOMMU (`AppleS5L8930XDART`) [C]

Four streams, each a 256 MB window: 64 segment entries (STEs) of 4 MB, each
pointing to a 4 KB page table of 1024 page entries.

| Offset | Use |
|---|---|
| `0x00` | command: `stream << 8 | index << 22 | 5` writes STE (value in `0x08`), `| 4` reads it into `0x08`; bit 3 busy; `0xfffff02` flushes every TLB, `streams << 8 | 0x7fff002` some |
| `0x08` | STE value |
| `0x0c` | one byte per stream: bit 7 translation on; `0xbb` for a stream in use |
| `0x10` | write `0x3f`: errors cleared |
| `0x18` | the invalid-translation target's address |
| `0x1000` | `0x7fffffff` at start; `0x1004`-`0x100c` = 0 |

- STE = page table's physical address `& ~0xfff | 1`.
- PTE = page's physical address `& ~0xfff | 1`.

The ISP's mapper names streams 0-3; iOS keeps them identical.

## On the iPad (2026-11-10): milestone 1

**Power.**  The ISP and its IOMMU hang off one PMGR power state, HPERF-ISP
(ADT clock id 45, `0x3f101048`), which iBoot leaves off (`0x300`); its
clocks, ISP-CLK (`0x3f10007c`, `0x80000004`) and HPERF-RT (`0x3f10006c`),
already run.  Switched on the usual way (`(v & ~0x10f) | 0xf`, settles at
`0x2ff`/`0x3ff`) the ISP answers (`0x40000` reads `0x00010000`), and so does
the IOMMU -- with iOS's last configuration still in it (`0x18` =
`0x30000000`, the last command a TLB flush): the gate only stops the clock,
so the IOMMU has to be programmed from scratch.  Off, both read whatever the
fabric last drove, without faulting.  The ISP's `function-device_reset`
(PMGR `ARST` 0x12) is the same register's index: the reset is a pulse on its
bit 31.

**Memory.**  For the tool the DT reserved 32 MB at `0x9d600000` (no-map;
the driver takes CMA memory instead, milestone 4), reached through
`/dev/mem` uncached; the tool refuses to run when `/proc/iomem` shows it as
the kernel's RAM.  The IOMMU maps its first 30 MB at address 0 on all four
streams; its invalid-translation target is a dummy page in the region
(iOS's `0x30000000` is the SDIO controller here).  The firmware's buffer is
at IOMMU address 0, so the ISP CPU's addresses (all relative to it) are the
region's offsets.

**The handshake, as it ran:**

| Step | Seen |
|---|---|
| boot address `0x80000000`, run | the wake interrupt within 10 ms |
| mailbox at wake | 7 channels, queue memory `0xaf40` + 1 bytes, heap `0x360000` (3.5 MB) wanted |
| the answers (firmware base 0, queues in the shared region at `0x200000`, the heap at `0x400000`, sensor type 6, the descriptor) | the second interrupt: the channel table at `0x200000` |
| `0x8042006` in mailbox word 3 | cleared at once -- once the host's queues are set up (below) |

| Channel | Type | Source | Entries | Queue |
|---|---|---|---|---|
| TERMINAL | 2 | 0 | 512 | `0x200700` |
| IO | 0 | 1 | 8 | `0x208700` |
| DEBUG | 0 | 1 | 8 | `0x208900` |
| BUF_H2T | 0 | 2 | 64 | `0x208b00` |
| BUF_T2H | 1 | 3 | 64 | `0x209b00` |
| SHAREDMALLOC | 1 | 3 | 8 | `0x20ab00` |
| IO_T2H | 1 | 2 | 8 | `0x20ad00` |

Every queue entry is 0x40 bytes with a state word first.  The firmware sets
its own queues' entries to 1 (TERMINAL's to 3); **the host's queues (type 0)
are the host's to set up**: each entry state 1, the next two words 0 (iOS's
channel set-up, `0x807e4e14`).  Without that the firmware never takes the
ready magic.

**Its first message**: right after the handshake, interrupt status bit 7 and
SHAREDMALLOC's entry 0 turned to state 0 with `0x00010000`, `"GOL\0"`: the
firmware asks the host for 64 KB for its log.

## On the iPad (2026-11-10): milestone 2

**The IPC rings** (AppleH4CamIn's little library, `0x807e4e14`-`0x807e5210`;
dispatch `0x807dd8d4`) [C, and run]: every entry's word 0 bit 0 says whose it
is.  A host channel (type 0) is the host's while bit 0 is 1: words 1-2 =
arguments, then word 0 = an address with bit 0 clear; the firmware hands it
back with bit 0 set.  The firmware's channels (types 1, 2) are the other way
round: bit 0 clear is a message (word 0 & ~3 an address, words 1-2
arguments), answered in place -- words 1-2, then word 0 = answer | 1 -- or,
TERMINAL (type 2), just released.  The doorbell is ISP `0x41020` =
`1 << (source + 4)`; the firmware raises the same bits in `0x41000`, which
the host clears by writing them to `0x41024`.

| Channel | iOS's handler | What |
|---|---|---|
| SHAREDMALLOC | `0x807ddfd4` | address 0 and a size: allocate (at least 4 KB), answer its address; else free |
| TERMINAL | `0x807de21c` | a NUL-terminated string at the address: `ISPCPU: %s` |
| IO, DEBUG | completion | a command's packet, answered in place |
| IO_T2H | `0x807de0c0` | notifications: `0xe00` frame-done timeout, `0xe01` focus driver failed |
| BUF_T2H | `0x807de324` | buffers back from the firmware (frames) |

**Commands** (`ispCmdSend`, `0x807e01a0`): a packet -- u16 opcode at +4, u16
result at +6 (0 = done), parameters from +8 -- copied into the shared
region, its address and length posted on IO.  Run: START (0), PRINT_ENABLE
(4, 1), CH_INFO_GET (`0x10d`, channel; 0xa8 bytes back): each answered in
10 ms.  CH_INFO_GET's answer: +0x20 the sensor's ID, +0x60 its number of
modes (iOS: 0 = no sensor), +0x4e the number of MIPI frequencies; iOS then
reads every mode with `0x106` and every MIPI frequency with `0x11a`.

**The firmware's banner**, once it has its log buffer: `HERA-APPLE`, HW
`0001004a`, its CPU at 205.2 MHz with separate write-back caches, our memory
map exactly (2 MB main memory at 0, its 3.37 MB heap at our `0x400000`, the
254 MB shared window from `0x200000`), `adc [release]`, `H4ISP-12.50.00`,
built with the iOS 7.0.3 toolchain.  Its memory requests: `LOG` 64 KB, then
`CSIM` 4 KB, 28 KB and 4 KB, and with sensors present another `CSIM` of
1 970 176 bytes.

**The sensors** (iOS's EnableSensor, `0x807de96c`): their supply, PMU LDO
10 -- the D1946 driver's LDO table (12-byte entries at `0x80e9eec8`): enable
`0x21` bit `0x80`, voltage `0x39`, 1700 mV + 50 mV a step, iOS's 23 =
2.85 V -- is on as iBoot leaves it.  The pins, `0x3fa00000 + 4 * pin`, as
iBoot leaves them: 192-195 plain outputs, the sensors disabled.  Clock pins
193 and 195 to the peripheral function (bit 5; iOS's "configuration 3" --
no pin here uses another), 1 ms, then the rear enable (192) high and the
front one (194) low (the ADT's polarity flags), 5 ms -- and CH_INFO_GET
finds **`0x9726` on channel 0 with 3 modes and `0x7739` on channel 1 with 1
mode**, the IDs of iOS's own tuning file.  Without that, 0 modes on both.

## On the iPad (2026-11-10): milestone 3, pictures

**The commands**, all named by the firmware itself: it keeps a table of
(opcode, name) pairs, 409 of them (`CISP_CMD_*`).  User space
(`H4ISPServices`, dyld cache `0x2928a000`; `H4ISP.mediacapture` has its own
copy of the same code) sends them through the kernel's method 0xb:

| Step | Command | Parameters (from +8) |
|---|---|---|
| set-file | CH_SET_FILE_LOAD `0x111` | channel, address, size |
| modes | CH_CAMERA_CONFIG_GET `0x106` (0x60 back) | channel, mode |
| mode | CH_CAMERA_CONFIG_SELECT `0x107` | channel, mode |
| crop | CH_CROP_SET `0x801` | channel, x, y, width, height |
| output | CH_OUTPUT_CONFIG_SET `0xb01` (0x28) | channel, width, height, range, format, stride(s) |
| recycling | CH_BUFFER_RECYCLE_MODE_SET `0x10e`, _START `0x10f` | channel, 1 / channel |
| buffers | BUF_H2T message | below |
| start / stop | CH_START `0x100`, CH_STOP `0x101`, CH_BUFFER_RETURN `0x104` | channel |

CAMERA_CONFIG_GET's answer: +0x10 the mode's width and height (u16 each),
+0x1c its frame rate in 1/256 fps, **+0x20 the channel's picture pool** (2
rear, `0xa` front), +0x3c and +0x48 more pools (4/6, `0xc`/`0xe`).  The
modes: rear 1292x728 at 29.97 fps (720p), 1292x808 at 27.6, 640x360 at
29.97; front 656x488 at 29.96.  Format 1 is one plane, 2 bytes a pixel,
stride `(2w + 15) & ~15`, **YUYV** (Y0 Cb Y1 Cr -- measured); other formats
are two planes with stride `(w + 15) & ~15` each.

**The set-files**: `H4ISPServices` picks one per sensor (`0x292a4ed0`) by
the sensor's ID, the ADT's sensor-type and two module IDs from CH_INFO_GET
(+0x14, +0x18; 1 and 1 on this iPad): the rear's `1011_04XX`, offset
`0x88000` in `H4ISPSETFILE`, 0x4efc bytes; the front's `0011_01XX`, offset
`0x97000`, 0x4840 bytes (their first bytes name them: `10 11 04`, `00 11
01`).  They load as CH_SET_FILE_LOAD right after START.

**Buffers** (the kernel's ISP_SendBuffers, `0x807e05a0`): a 0x180-byte
message -- type 1, the count, then 10 words a buffer: plane addresses 0-2,
0 x3, the plane count, the pool, the host's tag, 0 -- posted on BUF_H2T as
(address, 0x180, `0x30000000`).  Filled frames come back on BUF_T2H in the
same layout with type 2 (type 1: returned unused).  The picture pool's
number is the only thing that matters: buffers sent to pools 0 or 1 sit
there, no frame, no error.  The receiver's other pools (metadata,
statistics) are not needed to get pictures.

**Measured**: the first frame 70-80 ms after CH_START; 60 frames in a row
with each buffer handed straight back; the firmware's own exposure and
white balance settle within the first frames (the set-files are loaded).

## On the iPad (2026-11-10): milestone 4, the V4L2 driver

`drivers/media/platform/apple/apple-h4isp.c` (`apple,s5l8940x-isp` in
`dts/k93ap.dts`, built in: `config/k93ap.config`) is `k93isp.c` in the
kernel:

| Piece | How |
|---|---|
| power | HPERF-ISP as a PMGR clock gate (`isp_ps`), the reset pulse on its register's bit 31 (the node's third window), the sensors' supply LDO 10 as a regulator (`camera_ldo`, PMU 0x21 bit 0x80), their clock pins through pinctrl states (`active`: periph, `idle`: GPIO driven low), their enables as GPIOs (192 active high, 194 active low) |
| memory | coherent DMA memory (CMA) for everything the firmware has, mapped page by page at its own IOMMU address: the image at 0, the shared region at `0x200000`, the heap at `0x400000`, the rest (SHAREDMALLOC answers, set-files, buffer messages) from a `genalloc` pool; the 64 page tables (256 KB) and the dummy page held from probe.  The DT's 32 MB no-map region is gone |
| frames | vb2 scatter-gather buffers, any pages, mapped into the IOMMU at `buf_init`; CPU-cached for the applications, synced by vb2 |
| the rings | the firmware's answered from a threaded interrupt (IRQ 7, the ISP's index 0 -- iOS's `filterInterruptEventSource` takes index 0); the commands wait for their entry, answering the firmware meanwhile; a buffer posted on BUF_H2T as soon as it is queued, one message each, a message slot each ring entry |
| the IOMMU's faults | IRQ 117: `ERROR_STATUS` bit 31, the address in `ERROR_ADDRESS`, cleared with `0x3f` (iOS's `_dartFatalException`); counter overflows written back |
| streaming | the ISP booted at STREAMON (power, IOMMU, firmware, handshake, START, both sensors' CH_INFO_GET and set-files: about half a second, by the firmware's log), the channel set up as milestone 3, CH_START; at STREAMOFF CH_STOP, CH_BUFFER_RETURN, CISP_CMD_STOP, everything powered down and freed |
| firmware | `apple/h4isp-firmware.bin` and `apple/h4isp-setfile-<sensor ID>.bin`, out of the IPSW by `scripts/extract-h4isp-firmware.py` (`./cascadia k93 firmware`; the dyld cache read in place, every file checked by SHA-1), kept in the image and copied into the root at boot (`initramfs/init`) |
| debugging | `fw_log=1` (`/sys/module/apple_h4isp/parameters/`) prints the firmware's own log; debugfs `h4isp/cmd` sends a command by hand while a camera streams ("OPCODE LEN WORDS..." in hex, the answer read back), `h4isp/memory` lists what the ISP holds |

**Measured**: 640x480 from the front at 29.96 fps (300 frames in 10 s); the
rear's 1280x720, 640x360 and 1280x800 all stream, at half their rate in a
dark room (below) and 720p at 30 fps with the floor raised.  While
streaming 720p the driver holds 7.6 MB of the ISP's (11 blocks: the image,
the shared region, the 3.4 MB heap, the firmware's 64 KB `LOG` and its
`CSIM` blocks, 1.9 MB of them, the set-files); idle, nothing, and its
address space all free again after dozens of runs.  Two cameras at once:
the second STREAMON gets EBUSY.  `kill -9` of a reader mid-stream: vb2's
release stops it and the ISP is off (`0x300`).

**The frame rate in low light**: the rear's three modes gave half their
rate in a dark room.  The firmware's exposure has a floor and a ceiling for
the frame rate -- CH_AE_FRAME_RATE_MIN/MAX_GET `0x209`/`0x207`, (channel)
in, the rate in fps * 256 at +0xc: `0xefc` and `0x1df8` for mode 0 --
which iOS's `H4ISPFrameReceiver::startReceive` sets from its session
(`0x20a`/`0x208`, channel and rate, 0x10 bytes).  Left at the floor, as
iOS leaves it, the rate halves when the light is low; the driver's
`exposure_auto_priority` 0 raises the floor to the mode's rate (30 fps
in the same room).

**Orientation** (seen live on the iPad's screen, 2026-11-10): with the
desktop upright (landscape, as iBoot's scanout), the rear picture is
upright and the front one too, not mirrored -- a raised right hand on the
left, as V4L2 has it.  `camera_sensor_rotation` reads 0; the milestone-3
"upside down" was the iPad's position.

## On the iPad (2026-11-11): the picture controls

How: each command's layout from its sender in iOS (`H4ISPServices`, and the
copies in the camera plugin), then its effect measured on frames while it
alone changed (sent through debugfs `h4isp/cmd`, the newest frame kept every
half second): the mean and spread of the luma, the colour's mean distance
from grey, the mean |Laplacian| of the luma ("detail").  Where iOS never
sends a command, the firmware itself was decompiled (Ghidra, the image at
0, Thumb-2 from `0x1000` to `0x9fc40`).  The firmware has no GETs for the
back end's values (`0xa00`, `0xa08`, `0xa0a`, `0xa0c`, `0xa0e` answer 1) and
the scaler's have none at all, so the neutral values are the ones that
leave the frames as they were -- on both cameras.

| V4L2 control | Command (channel at +8, the value at +0xc) | What it does (measured) |
|---|---|---|
| `auto_exposure_bias`, -2 to +2 EV in thirds | CH_AE_BIAS_EXPOSURE_SET `0x204` (u16, then a u32 tag; 0x14 bytes) | the factor on the exposure's target, 8.8 fixed point: 0x100 the firmware's own (`0x203` reads it back), each doubling +1 EV (the mean luma x1.39, 2^(1/2.2)) |
| `brightness` 0-255 | CH_SCALER_BRIGHTNESS_SET `0xb02` (u8) | adds value - 128 to the luma |
| `contrast` 0-255 | CH_SCALER_CONTRAST_SET `0xb03` (u8) | (Y - 128) x value / 128 + 128: 0 is a flat grey |
| `saturation` 0-255 | CH_SCALER_SATURATION_SET `0xb04` (u8) | the colour x value / 128: 0 is grey |
| `hue`, -180 to 180 degrees | CH_SCALER_HUE_SET `0xb05` (u8) | turns Cb/Cr by (value - 128) x 180 / 128 degrees |
| `sharpness` 0-255; 128, the default, the firmware's own | knob 2: KNOB_MANUAL_CONTROL_ENABLE `0xa13` (knob), then CH_SHARPNESS_SET `0xa09` (u8); KNOB_MANUAL_CONTROL_DISABLE `0xa14` (knob) back to the firmware | detail 2.5 at 0, 5.9 at 0x80, steep above (22 at 0xc0, 70 at 0xff, mostly noise); the firmware's own in daylight is about 0x76 |
| `power_line_frequency` 50 Hz, 60 Hz, Auto | APPLE_CH_AE_FLICKER_FREQ_SET `0x8208` (u32: Hz, 0 to detect) | below |

All four of the scaler's are 128 when untouched, and any value is accepted.

**The knobs.**  The back end has seven light-adaptive "knobs"
(`CISP_KNOB_*`, in iOS's dump of `laKnobStatus`): 0 noise reduction
(`0xa0b`), 1 colour saturation (`0xa01`), 2 sharpness (`0xa09`), 3 colour
suppression (`0xa0d`: level and two thresholds, a byte each), 4 bad-pixel
correction, 5 DRC strength, 6 chroma noise reduction.  Each follows the
light level until KNOB_MANUAL_CONTROL_ENABLE `0xa13` (channel, knob; above
6 refused) hands it to the host, and `0xa14` gives it back; a SET without
the ENABLE is answered and changes nothing (saturation `0xa01` swept 0-0xff:
no change).  iOS sends its own saturation (`0xa01`, its property & 0x7ff >>
3) and sharpness (`0xa09`, property x 8) through the knobs, and its tables
per light level (`CameraSetup.plist`: `SharpnessLightLevel` /
`SharpnessValue` and the like per sensor -- how they reach the firmware is
not traced; CH_BE_LASETTING_SET `0xa15` is the likely command).  Linux
leaves the firmware's own tables.  The driver's saturation is the
scaler's (the same effect, linear, and the knob stays with the light); its
sharpness is the knob, the firmware's own until set.

The scaler's sharpness, CH_SCALER_SHARPNESS_SET `0xa07` (iOS: property x
10; `ScalerSharpBright` 2 and `ScalerSharpDark` 1 in iOS's tables), is a
filter level: 0 off (the default), 1 the softest (detail 2.8 against 5.2),
about 6 neutral, 10 and above the sharpest (15.3).  Not a control.

**Anti-flicker** (decompiled: iOS never sends `0x8208`).  The command posts
message 0x3c to the channel's AE controller (made by `0x0000b41c`; its
message table at `0xb2060`), whose handler (`0xba24`) keeps the word at
+0x218.  Every frame the controller (`0x0000bc44`) hands the AE
(`0x00018f74`) that frequency, or, when it is 0, its flicker detector's
(`0x0002765c`): 60 Hz at first, the other one when the rows show bands --
measured only when the exposure is long enough to see them.  The AE keeps
the exposure to whole periods of the light (its +0xe0: the 46.7 MHz pixel
clock / 2f).  0 is the firmware's default and the control's (Auto); 50 or
60 fixes it; there is no "off".  In daylight the frames showed no bands
either way.

**In the driver**: a control set at any time is kept, sent each time the
camera starts (after OUTPUT_CONFIG_SET, before the buffers: saturation 0
set before STREAMON gave a grey first frame) and at once while it streams;
the values last until the next boot.  Checked on both cameras, live and at
start.

## The plan

1. ~~**The ISP's CPU boots**~~ ✓ (2026-11-10, `tools/camera/k93isp.c`): a
   reserved memory region in the DT, the IOMMU programmed through
   `/dev/mem`, the ISP powered, the firmware loaded and started, iOS's
   handshake through to the ready magic.
2. ~~**The messages and the first commands**~~ ✓ (2026-11-10): the
   firmware's requests answered, START, its log printed, both sensors found.
3. ~~**One frame** from each camera~~ ✓ (2026-11-10): front 640x480,
   rear 1280x720, YUYV, streaming continuously.
4. ~~**A V4L2 driver** in the kernel, so applications can use the
   cameras~~ ✓ (2026-11-10): `apple-h4isp`, both cameras, mpv and ffmpeg.
5. ~~**The picture controls**~~ ✓ (2026-11-11): exposure bias,
   brightness, contrast, saturation, hue, sharpness and the mains
   frequency, on both cameras.
6. Open: other sizes through the firmware's scaler (CH_SCALER_CROP_SET
   `0x80a`); a frame-rate choice (S_PARM through `0x208`); the format's
   range (OUTPUT_CONFIG_SET's range 1 gives Y below 16 and above 235,
   reported as full range); manual exposure, white balance and metering
   (CH_AE_MANUAL_MODE_SET `0x22b` and CH_AE_INTEGRATION_GAIN_SET `0x238`,
   CH_AWB_CCT_GET/MANUAL `0x304`/`0x305` -- the CCT read 4948 K indoors --
   and APPLE_CH_AWB_MANUAL_WB_GAIN_SET `0x831a`, APPLE_CH_AE_METERING_MODE_SET
   `0x8206`, modes 0-3); iOS's light-level tables for the knobs.
