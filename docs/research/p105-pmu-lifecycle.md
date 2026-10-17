# D1946 lifecycle, event handling and fault research

Research date: 2026-10-11. Firmware is the same official p105/iPad2,5
12H321 kernelcache documented in `p105-charging.md`, SHA-256
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
All addresses below refer only to that image. No hardware access occurred.

## Wake, sleep and quiesce are separate branches

The main Mach-O nlist symbol table supplies exact names:

| Kernel global | nlist symbol | D1946 imported slot | Dispatcher branch |
| --- | --- | --- | --- |
| `0x80405578` | `_gIOPlatformSleepActionKey` | `0x80ed3198` | `0x80ecc7d8` |
| `0x8040557c` | `_gIOPlatformWakeActionKey` | `0x80ed319c` | `0x80ecc88c` |
| `0x80405580` | `_gIOPlatformQuiesceActionKey` | `0x80ed3194` | `0x80eccd30` |

Dispatcher `0x80ecc6e0` compares the incoming symbol against these imported
globals. This establishes lifecycle context more precisely than naming bits
from their numerical values:

- **Wake:** reads pending event registers 0x01..0x09 through the actual
  `0x80ecc460` method; writes nine IRQ-mask bytes starting at 0x12 from
  cached object state; sets register 0x1c bit 0; clears register 0x3f bit 7.
  It also processes event bits, restores GPIO/provider/event-source state,
  and clears register 0x5b bit 6 in the inspected zero-event fixture.
- **Quiesce:** reads/discards nine event bytes; sets 0x3f bit 7; writes
  `88 ff ff ff ff ff ae ff ff` to 0x12..0x1a; modifies 0xaf and, for one
  sleep-type branch, sets 0x1b bit 1. Additional GPIO/provider actions
  form part of the same operation.
- **Sleep:** enters different event-source/mutex/provider/GPIO handling;
  the synthetic fixture does not perform the Wake/Quiesce 0x3f mutations.

Thus 0x3f bit 7 is changed in Wake/Quiesce context. Its analog meaning remains
unproven: this is not evidence that it alone enables charging or guarantees
a safe charging transition. Register 0x3f bit 6 is separately used by the
current setter for zero-current requests. These bits must not be conflated.

## Offline actual-instruction harness

`tools/power/emulate-pmu-lifecycle.py` executes the actual dispatcher, event
read and register-mutation instructions. Only PMU I/O and surrounding
provider/mutex/GPIO/quiesce/logging/event-source helpers are mocked. All
mock calls and synthetic PMU transactions are recorded. The default fixture
has no pending events and object field +0xe4 value 7; this field is a synthetic
cached system sleep type, not a measured PMU revision. Tests also exercise
field value 0. The harness's existing `variant_field` argument is a fixture
parameter; it must not be interpreted as a hardware variant.
The harness pins the kernelcache hash and never opens a bus or device node.

```sh
python3 tools/power/emulate-pmu-lifecycle.py build/firmware/kernelcache.12H321.macho
python3 tests/test-pmu-lifecycle-emulation.py
```

All six tests passed inside the existing resource-limited build image with
Unicorn 2.1.4. They cover preservation of unrelated bits across six starting
0x3f values, IRQ/mask transaction sizes, distinct Sleep behavior, conditional
additional transactions, refusal of a different image and injected read error.
These tests do not validate physical PMU state, actual interrupts, resume
timing or electrical charge protection.

## Object +0xe4 is the cached system sleep type

Initializer `0x80ed01c4` interns the exact property name
`IOPMSystemSleepType` (string `0x80ed218c`) and stores its symbol at global
`0x80ed467c`. Method `0x80ecd7b0` either clears object +0xe4 to zero on its
active branch, or queries the root-domain property through helper
`0x80ed203c`, casts it to `OSNumber`, calls its unsigned 32-bit value method
at virtual slot +0x50 and stores the result at `0x80ecd83c`. If that lookup
fails, it stores 2. This proves the field's software provenance independently
of the synthetic lifecycle experiment.

The contemporary public Apple XNU header names sleep type 2 normal sleep
and 7 deep idle. This is a reference for the enum, not a claim that the public
source matches every instruction of this firmware: the tested kernel identifies
itself as `xnu-2784.40.6~18`; the available header is XNU 2782.40.9,
revision `c2b85efb3cd2a727d605af388855308d90d0bd70`:
[IOPMPrivate.h](https://github.com/apple-oss-distributions/xnu/blob/c2b85efb3cd2a727d605af388855308d90d0bd70/iokit/IOKit/pwr_mgt/IOPMPrivate.h).
The conditional +0xe4 branches therefore distinguish system sleep policies;
they cannot establish a PMU silicon revision or charger-temperature bucket.

The injected Wake failure is particularly important: when reading 0x3f fails,
Apple's code does not abort the subsequent write. In the synthetic fixture it
uses the previous stack byte `0x13` left by the 0x1c transaction, then writes
`0x13` to 0x3f. This is a confirmed unchecked-error limitation of that isolated
path. A future Linux driver must preserve error handling and avoid mutations
based on unavailable register values; it must not copy this behavior.

## Charger initialization and temperature-slot limits

Source init `0x80ed0558` resolves the PMU object, caches 0x3f low six bits as
the current-code ceiling (+0x1d), and uses 31 if that particular read fails.
It resolves optional `usb-input-limit-calibration` data (+0x20). It copies
the first five bytes of `ichg-tbat-max` into +0x24..+0x28 when the property is
long enough; otherwise it reads the five current registers
0x4b/0x4d/0x4f/0x51/0x53. The previously tested current setter clamps each
of those five requested codes against its cached slot and the hardware ceiling.

For verified p105 ADT the five cached values are `00 00 3f 3f 3f`.
Their numerical effect is proven: the first two programmed slot currents stay
zero and the other three follow the bounded requested code. **The temperature
range associated with each PMU slot is not yet established.** This is separate
from the decoded generic charger 10..45°C temperature/voltage policy table.
No even-register threshold, voltage target, calibration coefficient or
temperature-bucket meaning has been invented from register adjacency.

The initialization registers two nine-byte event masks with callback
`0x80ed06a4`: `08 00 00 00 00 00 00 00 00` at `0x80ed2fdc` and
`10 00 00 00 00 00 10 00 00` at `0x80ed2fe5`, through `0x80ecd878`.
The callback can forward a generic charger notification and uses separate
status reads for another condition. Its complete suspend/current/fault
interaction remains to be reconstructed; static mask membership alone is not
proof that an interrupt has acknowledged safely or stopped charging.

## Fault logging and watchdog limits

Fault routine `0x80ecc004` reads registers 0xa1, 0xa3, 0xa2, 0xa0 and four
bytes at 0xa8..0xab. In mode 1 it publishes the stored value, **explicitly
writes four zero bytes to 0xa8..0xab**, and then reads 0x1d. Its branch for
0x1d bit 1 interns the label `over_temp`; other labels are collected into
`IOPMUBootErrorFaults`. This establishes a boot-fault reporting path and an
explicit log-clear side effect, not a runtime charger shutdown policy.
Calling the whole routine is not a read-only diagnostic operation.

Event reads at 0x01..0x09 have possible consumption/read-to-clear effects as
already documented; a public D1946 datasheet has not certified them. The
separate 0xa8..0xab write-clear behavior does not establish read-to-clear
semantics for either range. No unsolicited register dump should be added.

The image contains a separate `AppleD1946PMUWatchDogTimer` class: its start
path around `0x80ed1724` resolves `function-keepact` and logs that the hardware
watchdog is inactive if that function is unavailable. This does **not** prove
a charger watchdog timeout or provide permission to add periodic PMU writes.
Charge-voltage target, analog protection, battery-presence shutdown and the
complete restart/termination requirements remain open.

## Next bounded investigations

1. Trace 0x80ed06a4 through its two generic charger callbacks, including the
   exact source-attach/event-versus-status relationship and error behavior.
2. Resolve the IRQ-mask owners and boot current ceiling; distinguish actual
   p105 values from synthetic test fixtures. The +0xe4 owner is now traced
   to `IOPMSystemSleepType` above.
3. Search the initialization/boot chain for writes to the PMU temperature
   threshold and charging-voltage registers, using exact consumers rather
   than inferring from register names on unrelated PMUs.
4. Only after approved device access, match controlled iOS observations to
   the known software transitions. Active Linux charger callbacks remain
   disabled until detach/fault/role coordination is complete.
