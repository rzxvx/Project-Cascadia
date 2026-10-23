# P105 external host: EHCI/OHCI route and remaining initialization gaps

This is offline research from the verified iPad2,5 iOS 8.4.1 build **12H321**.
No device access, controller activation, PHY/register write, accessory connection,
or VBUS test occurred. This supplements [USB host research](p105-usb-host.md).
The existing isolated DWC2 host build is a compiler experiment; it has not
established the physical external host route.

## Input provenance and scope

- Official IPSW SHA-256:
  `ce6cd59a4a3271eccb93b31c8d40e94ad966112999576663b3ab235196154756`.
- Decrypted kernelcache SHA-256:
  `db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
- P105 ADT SHA-256:
  `5c346586116de1bb32606acb27cad3158bcaa8a1318a10ee9631de09884cfd72`.
- Cascadia baseline `a9bf28f20e5ce908fd5805e7d58b3ce9d9f0e4cf`;
  inspected local research integration `f028b79`, with the subsequent host-PHY
  error-handling correction reviewed separately in
  [PHY error handling](p105-usb-phy-errors.md).
- Linux v6.12 `adc218676eef25575469234709c2d87185ca223a`.

Firmware method names below are limited to corroborating class strings,
existing symbols, and previously traced PHY implementations. All numeric
addresses refer to this exact cache. Thumb function pointers have low bit 1;
disassembly addresses have that bit cleared. Factory/class vtable prefixes and
actual instance vtables are explicitly distinguished.

Ignored local disassembly reports under `build/usb-host-research/`:
`usb-ehci-leaf-disasm.txt`, `usb-ehci-arm-disasm.txt`,
`usb-ehci-arm-start-disasm.txt`, `usb-ohci-arm-disasm.txt`,
`usb-arbitrator-clock-disasm.txt`, `usb-ehci-base-disasm.txt`, and
`usb-armio-clock-xrefs.txt`. The last two are working reports whose selected
ranges change while investigating; the verified addresses below are the
stable research record. Original firmware bytes are not committed.

## Board topology: an external port distinct from WLAN

The ADT USB complex is physical `0x3f108000`. Its clock-gate list, in order,
is **[88, 89, 90, 91, 87]**, not the sorted list of IDs. Clock-mask bit positions
index that list. The `function-usb20_reset` property dispatches PMGR FourCC
`ARST` with little-endian argument `0x22` (34 decimal); that argument is
not a direct register offset. The original Performance reset action maps it
to PMGR `0x1000 + 4 * 0x22 = 0x1088`, pulses bit31 for one microsecond,
and restores the saved word. This is an EHCI-controller reset, separate
from PHY reset and accessory/VBUS power.

| ADT node | Physical controller / IRQ | Publication mask | Clock mask | Companion ID / port type |
|---|---|---|---|---|
| USB device core | `0x36100000` / 11 | `0x03` | `0x30011` | Peripheral core |
| EHCI | `0x36400000` / 12 | `0x0c` | `0x30012` | Companion ID 0 |
| EHCI external Port1 | Controller Port1 | Inherits controller | `0x30002` | Port type 2; current-limit raw value 50 |
| EHCI WLAN Port3 | Controller Port3 | Inherits controller | `0x30012` | Port type 3; current-limit raw value 250 |
| OHCI0 | `0x36500000` / 13 | `0x04` | `0x30016` | Companion ID 0; port count 1 |
| OHCI0 external Port1 | Controller Port1 | Inherits controller | `0x30006` | Port type 2; current-limit raw value 50 |
| OHCI1 | `0x36600000` / 14 | `0x10` | `0x10008` | Separate controller; not this external-port proof |

The current-limit numbers are properties, not measured current or proof of a
5 V provider. The ADT alone establishes declared topology. The following
actual firmware consumers strengthen the external-port interpretation.

## External Port1 is instantiated and follows host cable roles

`AppleUSBEHCIARM` starts at kext VA `0x80c8f000`, file offset `0xc47000`.
Its port-creation function `0x80c90594` iterates the provider's child nodes,
reads `port-type` and `port`, and rejects indices outside 1..16. Type 2 uses
factory `0x80c90ad4`, corroborated by the `AppleUSBEHCIRemovablePort` class
string. Types 3/4 use a separate virtual factory at slot `+0x480`.
Consequently the actual P105 type-2 Port1 takes the removable-port path;
WLAN type-3 Port3 takes the HSIC path. This is stronger evidence than node
existence alone.

The removable-port startup at `0x80c90b30` reads its `clock-mask`, retains its
parent USB nub and creates a PHY service client. Its message handler at
`0x80c90e68`, after active/message guards, compares the cable object with:

| Cable symbol | Verified global | EHCI comparison | Resulting software power request |
|---|---|---|---|
| `USBDevice` | `0x804daee4` | PIC pair `0x80c90e80/0x80c90e82`, import `0x80c920cc` | State 2 |
| `USBDeviceNeedsAuthentication` | `0x804daee8` | PIC pair `0x80c90e92/0x80c90e94`, import `0x80c920d0` | State 2 |
| Other cable objects | Other symbols | Default branch | State 0 |

Both requests use stub `0x80c91b2c` → import `0x80c92078` → kernel symbol
`IOService::changePowerStateToPriv` at `0x802f9f8c`. Stub `0x80c91b0c` → import
`0x80c92070` → `IOService::powerOverrideOnPriv` at `0x802fa138` follows.
These are IOKit power-state requests, **not direct VBUS or PMU writes**.
Authentication still needs its separate accessory path; matching a symbol
whose name includes authentication does not establish successful verification.

`AppleUSBOHCIARM` starts at VA `0x8087f000`, file `0x837000`. Its port factory
`0x8087feac` selects `0x80880348` when `port-type == 2` at
`0x8087ffc8..0x8087ffd0`. The removable-port message handler `0x808806d8`
compares the same two globals through imports `0x808810a4/0x808810a8` and
requests the same states. Its stubs `0x80880c88/0x80880c78` resolve the same
kernel power-state functions. Thus both declared companion Port1 paths react
to the same host-role cable classifications.

This establishes Apple's **software route selection** for EHCI/OHCI0. It
does not measure the physical Lightning pin route, identify the camera adapter
ID, prove a Linux host boot, or supply the missing external VBUS circuit.

## EHCI actually requests the shared PHY host transition

`AppleS5L8930XUSBEHCI` is VA `0x80c96000`, file `0xc4e000`. Its actual
instance constructor `0x80c96ac8` stores vptr **`0x80c97058`** at
`0x80c96ae6`, verified from PIC load/add `0x80c96ae0/0x80c96ae2` plus 8.
Important instance dispatch slots are:

| Slot | Thumb target | Verified purpose |
|---|---|---|
| `+0x168` | `0x80c915c1` | Synopsys subclass startup |
| `+0x350` | `0x80c96b5d` | Subclass wrapper around base controller transition |
| `+0x354` | `0x80c96bd5` | Complementary subclass transition wrapper |
| `+0x35c` | `0x80c90595` | ARM port creation |
| `+0x474/+0x478` | `0x80c907cd/0x80c9080d` | Clock-client locking/request helpers |
| `+0x480` | `0x80c96b55` | HSIC port factory bridge |

Synopsys startup `0x80c915c0` collects child port types 3/4 into a bitmask,
using `1 << (port - 1)` and storing at object `+0x1584`. P105 WLAN Port3 gives
mask **4**; external type-2 Port1 does not enter that mask. Its static base
startup dispatch resolves class prefix `0x80c92120`, slot `+0x170`, to
`AppleUSBEHCIARM` startup **`0x80c902cc`**.

That ARM startup dynamically checks its PHY provider, retains it at
`object+0x1574`, reads controller `clock-mask`, obtains its clock client, and
calls PHY instance slot **`+0x34c` with argument 1** at
`0x80c90444..0x80c9044a`. Actual PHY vtable `0x80cad0a8` resolves that slot to
**`enableHostMode`, `0x80cac200`**. Transition wrappers `0x80c96b5c` and
`0x80c96bd4` call the same slot with arguments 0/1 around inherited controller
transitions. This is direct firmware evidence that external host startup
involves more than enabling an EHCI gate.

The recovered host method chooses ADT host tuning at PHY `+0x40/+0x44`,
asserts reset at `+0x08` bit 0, and releases it only under the documented PHY
power-state conditions. It also conditions a `+0x64` operation on HSIC state.
See the exact mapping and delay evidence in [PHY research](p105-usb-host.md#exact-phy-mapping-recovered-without-enabling-it).
The full provider/reset/ownership sequence remains incomplete and is not an
executable initialization recipe.

OHCI startup `0x8087fc4c` retains its PHY at `object+0x5f4`, carries the
`companion-id`, and, on the `generic-ohci` path, waits for `AppleUSBEHCI`.
At `0x8087fdb4..0x8087fdc8` it requests PHY slot `+0x348` with argument 1,
then `+0x34c` with argument 1. The exact actual PHY targets are
`0x80cac14d` (`enableDeviceMode`, PHY `+0x1c` bit 1) and `0x80cac201`
(`enableHostMode`). The first method's name alone must not be interpreted as
switching the whole connector back to peripheral mode; both calls appear in
the same OHCI startup sequence.

## Clock masks, actual callbacks, and power gates

The P105 arbitrator instance constructor `0x80b3934c` writes actual vptr
**`0x80b3b0c8`**, verified at PIC pair `0x80b39354/0x80b39356` plus 8.
Its instance `+0x344` points to `0x80b3a335`, the clock-enable client method.
Instance `+0x348` points to inherited `0x80864871`, a separate **power-gate**
client membership method, not the inverse of `+0x344`. It manages 16 sets at
object `+0x74..+0xb0` and dispatches provider `+0x358` on first/last membership.
That actual AppleARMIODevice target `0x804ae0b8` consumes `power-gates`; its
property parser is `0x804ae382..0x804ae3b6` (PIC pair
`0x804ae384/0x804ae38a` resolves literal `power-gates`). Both pinned P105 ADTs
have no `power-gates` on usb-complex. With count zero, the actual provider
returns `0xe00002c7` without downstream dispatch, and the arbitrator ignores
that result. This is one specific absent provider path, not proof that no
accessory/VBUS power callback exists elsewhere.

Instance `+0x34c` points to `0x80b3a78d`, which changes
USB-complex bit `0x80`; it must not be mislabeled as the disable-clock method.

The subclass clock-enable method maintains three extra client sets for mask
bits `0x10000/0x20000/0x40000`. It changes USB-complex clock-off bits
`0x40/0x20/0x10` according to client transitions, and delegates the low mask
to base `0x80864648`: PIC `0x80b3a626/0x80b3a62c` → import `0x80b3b084` →
class prefix `0x80866ad0`, slot `+0x34c` → target `0x80864649`.

The base method manages client sets indexed 0..15. Before/after set counts
are compared; empty/nonempty changes invoke the retained provider at
`object+0x64`, slot `+0x354`, with state and index. This provider is an
**AppleARMIODevice**, not the USB PHY: its class string is `0x804ca51d`,
constructor `0x804adc0c` stores instance vptr `0x804d2bb8`; the provider cast
uses import `0x80866134` → metaclass reference `0x804d2ba0`.

The provider's `+0x354` resolves **`0x804ae034`**. This checks the requested
index against the `clock-gates` property count, loads `clock-gates[index]`,
and dispatches platform slot `+0x350` with the resolved gate ID and state.
Property parsing at `0x804ae34c..0x804ae380` retains that ADT list and count.
Therefore the low bits resolve as follows:

| Client | Low clock-mask bits | Gate IDs |
|---|---|---|
| DWC2 | 0, 4 | 88, 87 |
| EHCI | 1, 4 | 89, 87 |
| EHCI removable Port1 | 1 | 89 |
| OHCI0 | 1, 2, 4 | 89, 90, 87 |
| OHCI0 removable Port1 | 1, 2 | 89, 90 |

The separate PMGR investigation maps IDs 88/89/90/91 to physical
`0x3f101084/0x3f101088/0x3f101090/0x3f101094`; ID87's ADT register index is
zero, so it does **not** justify another guessed write. See
[PMGR gates and actual firmware switch](p105-pmgr-gates.md).
This reference-counted clock path and the cable-change USB20 reset path are
not yet reproduced completely in Linux. The existing driver enabling EHCI
and OHCI0 gates is a subset, rather than a firmware-equivalent role switch.


## Port power callback: controller request, not a proven 5 V source

The removable-port factory at `0x80c90ad4` stores actual instance vptr
**`0x80c92608`**; PIC pair `0x80c90af4/0x80c90af6` plus 8 resolves it.
The inherited IOUSBFamily `setPowerStateGated` implementation at `0x8082b4f8`
dispatches the direct off→on transition (requested state 2, current state 0)
to instance slot `+0x384`, target **`0x808487f8`**. It performs two register
updates through the actual instance register helpers:

| Step | Actual firmware instruction range | Synthetic effect |
|---|---|---|
| Read without RW1C acknowledgment | `+0x3d8` → `0x808497a0` | Read mapped pointer `object+0xd8`, clear write-one-to-clear mask `0x2a` from the value to write back |
| Power request | `0x8084884e..0x80848862` | OR `0x1000`, EHCI `PORT_POWER` |
| Wake policy | `0x80848864..0x80848878` | OR `0x300000`, wake on connect/disconnect |
| Actual write | `+0x3dc` → `0x808497ac` | Store to the mapped PORTSC pointer, then `OSSynchronizeIO` |

The mapped pointer is retained by `0x80847b6c` at `0x80847bec`; controller
port creation at `0x80c9065a..0x80c90668` constructs its 4-byte mapping from
operational register base plus `0x44 + 4*(port-1)`. This confirms PORTSC access,
not an arbitrary PMU register. `PORT_POWER` is a real controller power request;
its physical connection to a P105 switch/boost or adapter still needs proof.
No PMU or Tristar power API occurs directly inside this inspected on callback.
That observation cannot rule out separate accessory-manager or PHY power paths.

`scripts/emulate-ehci-port-power.py` executes the actual gated dispatch,
actual port power callback and actual read/write helpers against fake RAM.
It requires the exact cache hash and verifies the instance vtable slots.
Only external `_IOSleep` and `_OSSynchronizeIO` imports are intercepted:
stubs `0x8084ab7c/0x8084ab9c` resolve imports `0x8084d01c/0x8084d024` to actual
kernel symbols `0x802e4658/0x800b24c4`. Off→on uses no sleep. The inspected
on→off callback does two writes separated by `IOSleep(1)` (one millisecond).

Four regression tests in `tests/test_ehci_port_power.py` pass in the existing
network-disabled build image: on requests, preservation of unrelated fields,
off sequence and rejection of unpinned firmware/unreviewed transitions.
For example initial synthetic `PORTSC=0x2b` produces writes `0x1001` and
`0x301001`, keeping the connection bit while avoiding accidental RW1C acks.
The emulator's PORTSC is ordinary RAM: it does not model hardware W1C behavior,
external voltage, IOKit asynchronous completion, PHY power, or USB enumeration.
Its result explicitly reports `vbussource_or_voltage_verified: false`.
Pinned [EHCI register definitions](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/include/linux/usb/ehci_def.h)
corroborate the standard power/wake bit meanings.

## Concrete Linux gaps and consequences

| Item | Current Cascadia implementation | Evidence / remaining work |
|---|---|---|
| External controller choice | Default DWC2 peripheral; isolated forced-DWC2 host experiment | Firmware publication/mux and removable Port1 follow EHCI/OHCI0 host roles. DWC2 forcing alone does not reproduce this route. |
| Host PHY transition | Host/HSIC PHY callback enables supplies and gates; shared OTG init preserves iBoot device tuning | Firmware EHCI startup explicitly requests shared PHY host tuning/reset. Role-aware shared PHY ownership is missing. |
| WLAN vs external port | EHCI enabled with WLAN child and HSIC mask 4 | Port1 is a separate removable path and is excluded from WLAN HSIC mask. Existing WLAN power is not external VBUS. |
| OHCI companion | OHCI0 DTS node disabled; no positive OHCI HCD/platform config | ADT and firmware associate companion ID 0 and matching external Port1 roles. Low/full-speed root devices may require OHCI. |
| Clock/reset lifecycle | PMGR EHCI/OHCI0 enable and HSIC bit; supply cleanup corrected | Firmware client clock masks, USB-complex clock-off state, USB20 reset coordination and shared PHY state transitions remain incomplete. |
| External power | No verified active VBUS provider implementation | LDO5 is an accessory rail with a firmware limit below a direct 5 V claim. Boost, external-adapter source, current protection and backfeed isolation remain unproved. |

EHCI root ports normally hand low/full-speed devices to a companion
controller. A high-speed USB 2 hub can instead expose those downstream devices
through its Transaction Translator and EHCI split transactions. This is a
controller architecture distinction, not a tested P105 hub workaround or a
power/backfeed recommendation. The architectural explanation in the
[Linux EHCI documentation](https://docs.kernel.org/usb/ehci.html) is old;
current pinned [EHCI hub code](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/host/ehci-hub.c)
also implements companion handover. Available generic
[OHCI platform code](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/host/ohci-platform.c)
is not proof that P105 needs only its Kconfig switch enabled.

The next safe work is offline tracing of the producer that turns actual
Tristar adapter identification into `USBDevice`/authentication cable objects,
controller power-state callbacks after the IOKit request, and the original
power-provider/boost path. No active EHCI/OHCI route, new host profile, reset
sequence, or runtime power write was added by this research.
