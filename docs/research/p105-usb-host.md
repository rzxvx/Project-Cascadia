# P105AP external USB host: evidence, preparation, and remaining blockers

Research date: 2026-10-11/12. Cascadia baseline
`a9bf28f20e5ce908fd5805e7d58b3ce9d9f0e4cf`, Linux v6.12
`adc218676eef25575469234709c2d87185ca223a`.
All work here is offline. No boot, USB role change, I2C/MMIO write, jailbreak,
firmware upload, host networking change or NAND access was performed.

## What the connector uses

The working external peripheral path uses the DWC2/OTG core at physical
`0x36100000`. Cascadia currently uses EHCI at `0x36400000` for the internal
HSIC path to the BCM4334 Wi-Fi chip. The actual P105 ADT also describes an
EHCI port 1 and its OHCI0 companion; therefore an assertion that EHCI has no
additional port is not supported. Whether that port routes to Lightning is
still a research question. Enabling EHCI alone does not establish external
host operation or VBUS power. The two current PHY outputs `[0] OTG` and
`[1] host/HSIC` must not be confused: the second's `hsic-supply` powers Wi-Fi,
not the connector's 5 V VBUS.

Upstream reports an HNP/SRP-capable OTG core with internal DMA, 14 host channels
and hub support. These are upstream observations, not measurements on our iPad.
The source still compiles DWC2 peripheral only and sets `dr_mode = "peripheral"`.
The USB console and CDC Ethernet share that one external controller.

Primary sources: [Cascadia connector discussion](https://github.com/rzxvx/Project-Cascadia/blob/a9bf28f20e5ce908fd5805e7d58b3ce9d9f0e4cf/README.md),
[board DTS](https://github.com/rzxvx/Project-Cascadia/blob/a9bf28f20e5ce908fd5805e7d58b3ce9d9f0e4cf/dts/p105ap.dts),
[PHY source](https://github.com/rzxvx/Project-Cascadia/blob/a9bf28f20e5ce908fd5805e7d58b3ce9d9f0e4cf/patches/files/drivers/phy/phy-apple-s5l-usb.c).

## Actual P105 firmware evidence

The verified official `iPad2,5_8.4.1_12H321_Restore.ipsw` supplied the encrypted
`Firmware/all_flash/all_flash.p105ap.production/DeviceTree.p105ap.img3`.
Local extraction and decryption used the public keys from the exact revision
already pinned by Legacy iOS Kit:
[key database af6bf5934dc61ed557a967a3f42ab7fb8ed8c45e](https://github.com/LukeZGD/Legacy-iOS-Kit-Keys/blob/af6bf5934dc61ed557a967a3f42ab7fb8ed8c45e/iPad2%2C5/12H321/index.html).
The full parsed ADT is 79,116 bytes, with zero unconsumed bytes, SHA256
`5c346586116de1bb32606acb27cad3158bcaa8a1318a10ee9631de09884cfd72`.
This is official firmware configuration for the model, not a live register dump
or proof that our unit's runtime state matches it.

| ADT fact | Value | Consequence |
|---|---|---|
| OTG PHY tuning, device | `0x549`, `0x2ff3` | Existing iBoot device state |
| OTG PHY tuning, host | `0x54f`, `0x6df3` | Exact P105 host values exist |
| PHY version/reference clock selector | 4 / 3 | Mode transition still requires register mapping |
| `usbhostset-en-incrx` | `0xe0` | Do not guess its register or apply it blindly |
| `/arm-io/i2c0/tristar` compatible | `tristar,ths7383`, `tristar,cbtl1608` | Early Tristar family, not the later CBTL1610 personality |
| Tristar `reg`, raw LE words | `0x1a, 0x9c4, 0, 0` | I2C provider data; not a Linux address binding by itself |
| Tristar interrupt, raw LE words | `0x56, 3` | GPIO interrupt description to resolve |
| `/charger` compatible | `charger,k48` | Apple's board charger orchestration participates |

ADT function dispatch resolves these chains:

- Charger `function-set_charger` → PMU phandle `0x28000000`, FourCC `chrg`.
- Charger `function-usb_dmonitor` → Tristar phandle `0x2b000000`, FourCC `DMON`.
- Dock `function-usb_dock_control` → charger phandle `0x5d000000`, FourCC `USBd`.
- Tristar `function-usb_dmonitor` → PHY phandle `0x36000000`, FourCC `DMON`.
- Dock `function-acc_pwr` → PMU FourCC `pmuL`, argument 5.
- Dock `function-acc_sleep_pwr` → PMU FourCC `pmuL`, argument `0x216`.
- Dock `function-acc_sw_en` → GPIO FourCC `GPIO`, arguments `0x1004`, `0x101`.

**These are dispatch identifiers and opaque arguments.** They are not verified
5 V boost controls, current settings or safe register writes. Accessory supply
is not automatically external USB VBUS.

Reproduce source extraction without any device or network access:

```sh
python3 scripts/extract-usb-adt.py /path/to/iPad2,5_8.4.1_12H321_Restore.ipsw
```

The script checks both the complete IPSW SHA256 and decrypted ADT SHA256,
refuses earlier evidence overwrite, and writes only under ignored
`build/usb-host-research/`. `adt-usb-power.json` includes raw property bytes,
phandle targets and FourCCs; it deliberately does not reinterpret charger tables
as proven voltage/current units. Previously generated evidence is under
`build/usb-host-research/adt-evidence-v4/`, which also includes HSIC and port-1 topology.

## What other projects provide

Linux v6.12 DWC2 already provides host channels, scheduling/split transactions,
hub handling, HID and USB storage support. It obtains optional `vbus-supply` in
`platform.c` and enables/disables that regulator in `hcd.c`. A missing regulator
does not establish that physical VBUS exists. A fake always-on regulator would
hide that missing electrical implementation, so none was added.

Sources: [DWC2 HCD](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/dwc2/hcd.c),
[role selection](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/dwc2/platform.c),
[DT binding](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/Documentation/devicetree/bindings/usb/dwc2.yaml).

OpeniBoot revision `866562fdb1cfd019bcd77885c80fbf0af65d5c15` has older A4,
S5L8900/8720/8920 platforms and USB charger identification code. It contains no
A5 P105 platform and no verified Tristar/D1946 external host-power sequence.
Its older PMU register numbers must not be copied onto P105.
[OpeniBoot platform code](https://github.com/iDroid-Project/openiBoot/tree/866562fdb1cfd019bcd77885c80fbf0af65d5c15).

Sandcastle's public status reports Tristar support on later devices. Its Linux
revision `0c2f7dda13d67bb7e06123df516f8bdb1000a79b` uses an A10 `hx,usbdev`
controller and different clock/power topology. The inspected Linux tree does
not contain a standalone P105/early-Tristar external-VBUS implementation. This
is architectural comparison, not an A5 driver ready to transplant.
[Sandcastle status](https://projectsandcastle.org/status),
[A10 board DTS](https://github.com/corellium/linux-sandcastle/blob/0c2f7dda13d67bb7e06123df516f8bdb1000a79b/arch/arm64/boot/dts/hx/hx-h9p-d10.dts).

The repository's A1455 boardview exposes PMU/Tristar VBUS net names, but A1455
cellular routing is not proof for A1432. No power-register implementation was
derived from it.

## Offline AppleTriStar tracing

The actual 12H321 kernelcache contains `com.apple.driver.AppleTriStar` at
VA `0x80884000`, size `0x9000`; file offset `0x83c000`.
The embedded Mach-O header at that offset validates the mapping. Its `__text`
is VA `0x80884950`, length `0x2f3c`; `__cstring` VA `0x80887b3c`, length `0x4c1`.

Manual checks of Thumb disassembly establish:

- Startup at `0x808853dc` resolves `function-usb_dock_control` into object field
  `+0x124` and `function-vbus_voltage` into `+0x130`.
- Wrapper `0x8088586c` invokes the former AppleARMFunction, passing a pointer to
  its caller's argument. This bridges to the charger's `USBd` handler.
- Wrapper `0x808859c0` invokes the voltage function into an output buffer, with
  a fallback virtual method. This is a measurement path, not a boost enable.
- `downstream-vbus` (`0x80885a08`) and `ignore-vbus` (`0x80885a40`) are property
  existence checks, not immediate VBUS control writes.

Ignored local reports: `build/usb-host-research/tristar-xrefs.json`,
`tristar-fields.txt`, `tristar-disasm.txt`. Heuristic xrefs were checked against
the listed disassembly; the broader role state machine remains unresolved.

### Tristar event, ID and role evidence chain

The actual **instance** constructor at `0x808869e4` installs vtable
`0x80889298`. Table `0x80889658` belongs to a metaclass and cannot supply
instance method slots. The early THS7383 reader differs from later CBTL1610
code; no later-chip CRC or charging-handshake interpretation is transplanted.

| Transition | Exact 12H321 evidence | Established limit |
|---|---|---|
| IRQ → ID read | `0x808870a0` reads register `0x0d`; bit `0x08` invokes `0x80885160` | Other IRQ bits are separate notifications, not adapter types |
| Register reply → ID bytes | Instance slot `+0x380` → `0x80886be8`: reads seven bytes at `0x39`, then one status byte at `0x0c`; requires status bit 7 and final register byte `0x75`; reverses the other six bytes | No CRC check appears in this reader; this is a register buffer, not a complete IDBUS wire packet or proof of authentication |
| ID bytes → manager | `0x80885160` invokes import stub `0x8088798c`; pointer `0x80888040` resolves `0x807139b8` in IOAccessoryManager; it invokes six-byte property consumer `0x807133d8` | Property publication does not itself establish USB host role |
| Manager handshake → charger API | The same BuiltIn owner reaches `0x80714028`; success/debug-allow branch `0x80714260` passes `0x80000`, failure branch `0x80714cee` passes `0x100000` to virtual slot `+0x36c` | API dispatch is established; complete permission, ordering and protection policy still require reconstruction |
| Accessory ID → AppleCableType | **Unknown** for a camera adapter | No verified camera-adapter ID or complete classification path yet |
| AppleCableType → controller publication | Verified symbol initialization and criteria consumer, detailed below | `USBHost` describes the attached host: iPad is peripheral; `USBDevice` describes attached device: iPad is host |
| Publication → SoC data mux | Firmware writes criteria bit 2 to USB-complex control bit 0 | Does not prove Lightning lane routing or safe role transition under Linux |
| Host role → peripheral power | **Unknown** actual camera-adapter source/conversion/protection architecture | Neither mux selection nor the 3.3 V accessory LDO supplies proven 5 V VBUS |

SystemID is separate: slot `+0x37c` → `0x80886ba8` reads three register bytes
at `0x20`, requires initial byte `0x74`, and combines
`((word >> 6) & 0x3fc) | ((word >> 16) & 0xff)` into a 16-bit result.
It is not the six-byte accessory ID. Slot `+0x388` reads status `0x0c` and
returns zero if bit 7 is clear, otherwise `1 + bit6`; the precise physical
meaning of those returned indices remains to be established.

The BuiltIn constructor `0x80885350` installs actual instance vtable
`0x80888998` using the PIC load/add pair at `0x80885368/0x8088536a` and
store `0x8088536e`. Its slot `+0x36c` resolves `0x8088586c`, the already
verified `function-usb_dock_control`/`USBd` AppleARMFunction bridge. This
establishes the handshake producer for the ID IRQ path with that same owner;
virtual slots on unrelated classes in IOAccessoryManager are not assumed
equivalent. The masks above are API arguments, not Tristar/PMU register values.

The companion [charging analysis](p105-charging.md#accessory-power-handshake-permission-and-refusal)
now tests the actual `USBd` consumer `0x804c70e4` against synthetic policy
state. Its flag group `0x1c0000` sets per-port source-inhibit field `+0x130`
and handshake-failure field `+0x132`: `0x80000` clears both, `0x100000` sets
both, and `0x40000` sets inhibit while clearing failure. Budget consumer
`0x804c8200` gives an inhibited port a zero contribution before remaining
current limits. This links the BuiltIn handshake outcome to **incoming-charge
policy**. It is not permission to source outgoing 5 V VBUS, and does not make
the complete power handshake, authentication or electrical protection usable
under Linux.

`0x80886d2c` accepts routing FourCC values including `usb0`, `usb1`,
`urt0`, `urt1`, `urt2`, `jtag` and `mbus`, translating them to Tristar
register 1. These identify requested internal data modes, **not** camera
adapter IDs or AppleCableType strings. The offline planner records the
transition table and leaves both classification and VBUS gaps explicitly
unresolved. It has no write backend.

Two ID-to-routing functions are now isolated in IOAccessoryManager:
`0x80713c84` produces data mode FourCCs from the first ID byte, and
`0x80713dc4` produces additional accessory-mode FourCCs from its top two bits
and, for one branch, byte 1. The actual-instruction offline tool
`scripts/emulate-accessory-routing.py` confirms that the independently
reported ordinary cable fixture `10 0c 00 00 00 00` selects `usb0` and no
second data mode. The second decoder maps top bits `01` to `urt1`/`urt1`,
`10` to `jtag`/`jtag`, and reserved `11` to empty outputs. These are function
outputs; actual physical pin assignments are not established here.
One data-decoder path invokes an opaque platform predicate at
`0x8071d56c`; its 0/1 return is an explicit synthetic fixture, never assumed
to match the running board. Three additional tests verify the ordinary
fixture, separate decoder behavior and fail-closed input validation.
This narrows ID-to-routing analysis without inventing a camera-adapter ID or
claiming that selecting `usb0` changes the USB role. Ignored output:
`build/usb-host-research/accessory-routing-emulation-v1.json`.

The additional `scripts/emulate-tristar-id.py` runs the actual THS7383 ID
reader instructions with fake I2C replies and RAM. Five tests confirm six-byte
reversal, status gating, marker errors, SystemID bit packing, unchanged output
on failure, early termination after failed reads, and pinned input validation.
Simulated status `0x80` and `0xc0` produce the same ID byte order; neither
proves an adapter role. Output is ignored local
`build/usb-host-research/tristar-id-emulation-v1.json`.

A [first-person Lightning reverse-engineering report](https://nyansatan.github.io/lightning/)
by nyansatan (2020) independently describes THS7383 on mini 1, measured
IDBUS traffic and a normal cable ID `10 0c 00 00 00 00`. It supports
investigating ID74/75, lane selection and separate power negotiation. The
author mixes personal measurements with internal-material interpretation and
explicitly notes uncertainty; it is not a verified THS datasheet. We do not
retrieve its linked leaked sources. Its later-chip observations and power
handshake outline cannot be assumed correct for this P105 firmware. No
camera-adapter ID is established by that report.

## Accessory power and USB VBUS are separate questions

The P105 dock's `acc_pwr` dispatch argument 5 is now decoded against the
**same 12H321** PMU firmware, rather than an older LDO table. `_setLDO` at
`0x80ece79c` uses 12-byte entries at `0x80ed2ec8`. Entry 5 is raw
`c40932101f02340021000400`:

| Field recovered from consumer | LDO index 5 value |
|---|---|
| Minimum / step / maximum code | 2500 / 50 / 16 |
| Maximum in firmware voltage units | 3300 |
| Voltage register / mask | `0x34` / `0x1f` |
| Enable register / mask | `0x21` / `0x04` |
| Optional shared register / mask | `0x7c` / `0x02` |

These values are consistent with a 2.5–3.3 V accessory LDO, not a 5 V boost;
voltage units and physical rail behavior are not measured here. The firmware
LDO function at `0x80ecf4a0` reads the dispatch argument's index byte, options
byte and voltage word before calling the setter. `acc_sleep_pwr` argument
`0x216` resolves index 22 with options `0x02`: its table entry has enable
register `0x26`, mask `0x04`, and no adjustable voltage fields. Neither is
permission to write a live PMU register.

The independent offline Unicorn experiment
`scripts/emulate-usb-accessory-ldo.py` executes the actual `_setLDO` instructions
with fake read/write callbacks and synthetic PMU RAM. It confirms normal
index-5 enable/disable changes only register `0x21` mask `0x04`, index 22 uses
`0x26` mask `0x04`, and options bit 0 selects the shared `0x7c` mask instead.
A requested voltage of 3300 is accepted; 5000 returns `0xe00002c2` before any
fake bus access. Read/write failures propagate and stop the operation. These
are simulated digital transactions, not measured voltages or power safety.
Output is `build/usb-host-research/ldo-emulation-v2.json`. Four firmware tests
plus the parser/profile/planner, Tristar and ID-routing suites total **28 passing
USB/accessory tests** in the existing unprivileged, network-disabled build image.
Native host Python runs 16 tests and skips the 12 requiring its absent Unicorn
dependency.

The statement that the iPad must directly generate 5 V for an external USB
bus is too narrow. Apple's public support documentation establishes that the
**Lightning to USB 3 Camera Adapter** has its own Lightning power input, which
can supply the iOS device and attached USB device. It also documents powered
hubs and says this adapter needs iOS 9.3 or later.
[Apple Support: USB-A camera adapters](https://support.apple.com/en-us/111811).
This is a documented iOS product behavior; Cascadia accessory detection and
power arbitration for that adapter are still unimplemented/unverified.

Apple's public **Accessory Design Guidelines R31, 2026-09-21**, section 26
(page 242), describes limited power drawn by Lightning accessories. Section
36 (page 254) separately describes accessories providing power to a device
and reporting an external source's capability.
[Official Apple guidelines PDF](https://developer.apple.com/accessories/Accessory-Design-Guidelines.pdf).
Local reference SHA256:
`b631b8b25d6b4fba85f90dff494a7a7edc9996e357cbc97093aedc9d9bd6f2c9`.

No primary teardown or schematic proving an internal boost converter in the
original Lightning-to-USB Camera Adapter (MD821/A1440) was found in this
research pass. Adapter-side conversion from the accessory rail is a hypothesis,
not an established replacement for a missing VBUS design. The audited 8.4.1
firmware also predates the documented 9.3 minimum for the powered USB 3 adapter;
its later accessory policy cannot be assumed identical. We have not selected,
purchased, connected or configured either adapter, and make no DIY wiring or
backfeed recommendation.

## External controller routing: additional evidence, not a working route

The official P105 ADT provides these additional topology facts:

| Node | Port / type | Publication criteria | Clock mask |
|---|---|---|---|
| `usb-ehci` | Controller | `0xc` | `0x30012` |
| `usb-ehci/wlan` | 3 / 3 | Internal Wi-Fi child | `0x30012` |
| `usb-ehci/usb-ehci-port1` | 1 / 2 | Child of EHCI | `0x30002` |
| `usb-ohci0` | Companion ID 0, one port | `0x4` | `0x30016` |
| `usb-ohci0/usb-ohci0-port1` | 1 / 2 | Child of OHCI0 | `0x30006` |
| `usb-device` | DWC2 peripheral | `0x3` | `0x30011` |

Port-1 nodes advertise a `port-current-limit` value of 50; units and runtime
policy are not established here. They do not prove physical connector wiring.

`AppleS5L8930XUSBArbitrator` is VA `0x80b38000`, file offset `0xaf0000`.
Its `handleStart` at `0x80b39908` maps provider register 0 and stores its
mapped address in `object+0x118` at `0x80b399f8`. The provider's ADT
`usb-complex` register is arm-io-relative `0xf108000`, physical `0x3f108000`.
`handleUSBCableTypeChange` at `0x80b3a0d0` derives publication criteria and
writes their bit 2 to **USB-complex control bit 0** (`0x80b3a238..0x80b3a24c`).
The cable-change thread at `0x80b3956c` quiesces child nubs whose criteria
include bit 2, changes that bit, performs USB20 reset dispatch, and restores
the nubs. This establishes a firmware controller-selection mechanism; the
precise cable types, reset dependencies, and physical Lightning route still
need tracing. USB-complex bit 0 is not a VBUS power command.

The cable symbols are now resolved from the AppleARMPlatform initializer,
not guessed from addresses. Each symbol creation call returns `r0`, which
is stored in the **following** iteration. `scripts/usb-phy-plan.py` validates
the actual Thumb PC-relative literal loads/additions, exact NUL-terminated
strings and `STR r0` targets:

| Apple cable type | Symbol global | Creation-result store | Criteria / control bit 0 | iPad role |
|---|---|---|---|---|
| Detached | `0x804daed8` | `0x804bbe88` | 1 / 0 | Disconnected |
| USBHost | `0x804daedc` | `0x804bbe98` | 3 / 0 | Peripheral connected to a host |
| USBHostAltConfig | `0x804daee0` | `0x804bbea8` | 3 / 0 | Peripheral |
| USBDevice | `0x804daee4` | `0x804bbeb8` | 5 / 1 | Host connected to a device |
| USBDeviceNeedsAuthentication | `0x804daee8` | `0x804bbed2` | 5 / 1 | Host selection, authentication not established |

Base `getPublicationCriteria` at `0x80864454` maps these symbols to 1, 3 or 5;
the P105 subclass at `0x80b3a300` ORs in HSIC bit 3 and optional OHCI1 bit 4.
This is concrete evidence that bit 0 controls device/host controller routing.
The ADT EHCI/OHCI0 publication masks include bit 2, while DWC2's mask includes
bit 1. It therefore points to Apple's external-host use of EHCI/OHCI0 rather
than forcing the Synopsys peripheral core into host mode. That conclusion is
an inference from the traced publication/mux behavior; physical Lightning
wiring and successful peripheral enumeration have not been measured.
The isolated DWC2 host build below remains a software experiment. Its role
setting alone does not reproduce Apple's controller selection.

Ignored disassembly reports are `usb-arbitrator-disasm.txt`,
`usb-arbitrator-map.txt`, `usb-arbitrator-base.txt`, `usb-symbol-init.txt`,
`usb-symbol-init-tail.txt` and `usb-symbol-xrefs.json` under
`build/usb-host-research/`. No active EHCI/DWC2 routing change was made.

## Implemented local preparation

### Exact PHY mapping recovered, without enabling it

The same kernelcache contains `AppleS5L8930XUSBPhy` at VA `0x80cab000`,
file offset `0xc63000`. Its startup reads the four exact ADT tuning properties
into object fields `+0x90/+0x94` (device) and `+0x98/+0x9c` (host).
`enableHostMode` at `0x80cac200` writes these to registers **`+0x40/+0x44`**.
The offsets are now verified from firmware rather than guessed from Samsung.

Its vtable at `0x80cad0a8` resolves register helpers:

| Vtable slot | Thumb target | Operation at mapped PHY base (`object+0x78`) |
|---|---|---|
| `+0x360` | `0x80cac33d` | Read register |
| `+0x364` | `0x80cac335` | Write register |
| `+0x368` | `0x80cac351` | OR bits |
| `+0x36c` | `0x80cac345` | AND mask |

After choosing host or device tuning, `enableHostMode` asserts register `+0x08`
bit 0. If object power level is at least 2, it delays **20 microseconds**, clears
that bit, then delays **1,000 microseconds**. The delay stub `0x80caca84` resolves
through pointer `0x80cad000` to the kernel symbol `_IODelay` at `0x802e46cc`, so
the units are established rather than inferred from constants. If the power
level is only 1, reset stays asserted until another state transition.

The method also toggles `+0x64` bit 1 **only when the AppleUSBPhy HSIC flag
at `object+0x50` is zero**. The base class start at `0x8086180c` reads boot
argument `hsic`, default 1, and recursively looks up `hsic-enabled` at
`0x80861898`. If both are present/enabled, it stores 1 at `0x808618ac`.
The P105 `/arm-io` node has the empty `hsic-enabled` property; consequently
normal firmware defaults skip this `+0x64` branch. This resolves an object
state flag, not a VBUS source bit or a live state measurement.
`enableDeviceMode` at `0x80cac14c` toggles `+0x1c` bit 1. Provider clock
callbacks, locking, port reset ownership and data routing need complete
tracing. This partial sequence is not ready to execute.

`scripts/usb-phy-plan.py` reads only SHA-pinned firmware and ADT, validates the
Mach-O mapping, register-helper vtable and `_IODelay` resolution, and prints
JSON with explicit power-state conditions and the resolved HSIC default. It contains no execution or register
access backend. The current PHY driver's wrong header claim `0xf8` and unused
guessed tuning offsets `0x30/0x34` were removed/corrected; runtime behavior was
not changed.

```sh
python3 scripts/usb-phy-plan.py build/firmware/kernelcache.12H321.macho \
  build/usb-host-research/adt-evidence-v4/DeviceTree.p105ap.raw
```

The extraction/parser path now rejects malformed IMG3 lengths, empty/misaligned
ciphertext, truncated ADT alignment padding, negative offsets, enormous node
counts, duplicate properties and excessive recursion. The official P105 tree
still consumes exactly 79,116 bytes and retains the same SHA256.

### Host build inputs and linker correction

`scripts/prepare-usb-host.py` creates an isolated host-only fragment and DTS
under `build/usb-host-research/profile/`. It enables DWC2 host, USB storage,
SCSI disk, USB HID and VFAT; disables the peripheral CDC gadget; changes only
DWC2's DT role. It adds no VBUS regulator, PHY tuning write or boot/upload
command. Default peripheral config, DTS and reference outputs remain separate.
Generator tests cover role drift, new unreviewed VBUS bindings, contradictory
symbols, and attempts to overwrite reference files or earlier evidence.

A real host-only linker issue was fixed in `patches/tree/0001-cascadia.patch`:
the Apple session-recovery call in `core_intr.c` referred to
`dwc2_s5l_new_session()`, which lives in `gadget.c`; host-only builds omit
`gadget.o`. Both the temporary variable and call are now guarded by Apple S5L
**and** peripheral/dual-role mode. Peripheral behavior is unchanged.

```sh
python3 -m unittest discover -s tests -p 'test_usb_*.py' -v
python3 -m unittest discover -s tests -p 'test_tristar_id.py' -v
python3 -m unittest discover -s tests -p 'test_accessory_routing.py' -v
python3 scripts/prepare-usb-host.py
```

The parent build workflow compiles this experimental configuration in an
isolated tree. The first isolated host kernel compile completed successfully (parent log
`logs/night/usb-host-kernel-build.log` outside the repository). It reported 22
existing prototype/unused warnings. A successful compile means source
consistency only; it does not remove the electrical and firmware blockers below. No host boot bundle is
approved for transfer.

## Remaining hardware blockers and validation order

1. **Independent tick:** the AIC 24 MHz clockevent must report PASS. Peripheral
   SOF fallback has no viable role in a host-only boot; frozen jiffies would
   break enumeration and timeouts.
2. **PHY transition:** complete Apple's clock and role transition sequence around
   the now recovered tuning/reset registers, including HSIC state and
   shared EHCI/OTG routing. Current driver deliberately preserves
   iBoot's device-mode PHY state and has no role-aware `set_mode` callback.
3. **Lightning routing:** reconstruct Tristar/accessory identification and the
   `USBd` path. Forcing DWC2 host does not prove the adapter has routed D+/D−.
4. **Power path:** determine the actual adapter architecture first: native
   source/boost, adapter-side conversion, or externally powered adapter. Resolve
   accessory supply, source budget, over-current protection, discharge and
   mutual exclusion with incoming power. Only model verified hardware and its
   true provider as a VBUS regulator; LDO 5 is not itself a 5 V VBUS supply.
5. **Hardware tests, only after explicit authorization:** begin with independent
   serial/glass access and a low-power keyboard; then a hub and read-only flash
   media. Confirm USB topology, interrupts, timer operation and battery current.
   Do not create automatic filesystem mounts or storage writes.
6. **Return to peripheral:** verify disconnect, power-down and device-mode
   restoration before considering runtime dual-role support.

A compliant externally powered hub can power downstream peripherals and reduce
the iPad's current demand. It does **not** by itself prove the iPad-side VBUS
detection, Lightning role selection or data routing works. Backfeeding hubs,
improvised Y cables, or simultaneous PC/host-source power are not acceptable
substitutes for the missing source/protection design. No measured performance
or external-host success is claimed.
