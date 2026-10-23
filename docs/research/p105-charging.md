# p105 charging: source evidence and offline experiments

Research date: 2026-10-11. Baseline Cascadia commit:
`a9bf28f20e5ce908fd5805e7d58b3ce9d9f0e4cf`. Local work is on
`research/p105-power-storage-usb`; no iPad operation was performed.

**Charging is not enabled by this work.** We now have concrete Apple firmware
control paths and a reproducible offline emulator, rather than only a list of
missing Linux subsystems. Hardware semantics, charger policy and protection
must still be validated before installing a charger driver that writes registers.

The [14 October early-P105 follow-up](#early-p105-initialization-follow-up--14-october-2026)
independently examines iOS 6.0. It distinguishes actual initialization writes,
temperature measurement trim and voltage-to-capacity profiles from the still
unqualified analog charging setup. The original sections below retain their
explicit 12H321 source scope.

## Existing support and useful fixes

The existing `apple-hdq-battery.c` is a fuel-gauge driver, not a charger driver.
Its HDQ commands read the standard bq2754x-family register pairs. The UART
controller necessarily sends HDQ request frames; no gauge register-write
command, data-flash change or charger setting is sent.

The gauge exposes battery voltage, signed average current, temperature,
remaining/full/design charge, state of charge and cycle count. The existing
`apple-pmu-i2c.c` provides an 8-bit-address D1946 regmap, GPIOs, regulator
children and power-off handling. Neither driver connects USB source detection
to a charging policy.

Local fixes to the gauge are independent of speculative charger programming:

- An unsuccessful refresh leaves the previous sample completely intact.
  Previously earlier fields were overwritten before a later read failed.
- A property is selected while the refresh mutex is still held. Previously
  another reader could start refreshing the cache between unlock and selection.
- Negative measured current reports discharging even if the gauge's DSG flag
  has not yet changed. This agrees with Linux's signed-current handling.
- A UART break that remains asserted after the deadline stops the transaction
  before command frames are sent.

`tests/test-hdq-battery.py` compiles the actual driver's functions with fake
UART I/O and gauge replies. It exercises framing, a stuck break, all nine
refresh-failure positions, cache expiry/wraparound, signed units, lock scope and
failure unlocks. Undefined-behavior sanitizer is enabled. No device is involved.

## Evidence inputs and provenance

Only the already verified official Apple IPSW is used for firmware evidence:

| Input | SHA-256 |
| --- | --- |
| `iPad2,5_8.4.1_12H321_Restore.ipsw` | `ce6cd59a4a3271eccb93b31c8d40e94ad966112999576663b3ab235196154756` |
| Decrypted `kernelcache.12H321.macho` | `db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c` |
| Decrypted `DeviceTree.p105ap.raw` | `5c346586116de1bb32606acb27cad3158bcaa8a1318a10ee9631de09884cfd72` |

This is static evidence from iOS **8.4.1**, not a live trace of our iOS 9.3.5
device. The addresses below apply only to that exact kernelcache. Older notes
mentioning `0x80c9...` PMU addresses do not match this file's layout.

The USB research work extracted the ADT using the pinned
[Legacy-iOS-Kit key database](https://github.com/LukeZGD/Legacy-iOS-Kit-Keys/blob/af6bf5934dc61ed557a967a3f42ab7fb8ed8c45e/iPad2%2C5/12H321/index.html).
The selected-node evidence is local `build/usb-host-research/adt-evidence-v2/adt-usb-power.json`.

No Apple firmware or binary dump should be committed or published with these
notes. The emulator contains addresses and analysis, and requires the user's
own local firmware file.

## Apple device tree: charger policy is board-specific

The p105 `device-tree/charger` node contains:

| Property | Observed value |
| --- | --- |
| `function-set_charger` | provider phandle `0x28000000`, type `chrg` |
| `min-charge-capable-current` | 1000 |
| `usb-input-limit-max` | 2100 |
| `charge-limits` | 2520, 1760, 1240, 720 |
| `ichg-tbat-max` | bytes `00 00 3f 3f 3f 00 00 00` |
| `charger-sleep-interval` | 300 |

The provider is `/device-tree/arm-io/i2c0/pmu`, compatible `pmu,d1946`, address
`0x3c`. The function type `chrg` is a dispatcher identifier, not a sequence of
literal register writes. In `AppleD1946PMU`, the branch at `0x80eccc80` compares
the type against literal `0x63687267` (`chrg`) at `0x80eccd24`, allocates a
0x2c-byte object, calls `0x80ed0500`, and stores the resulting
`AppleD1946PMUPowerSource` at PMU-object offset 0x104. This connects the ADT
charger function to the concrete PMU backend.

The charger node's 80-byte `charge-currents` property is now decoded against
the actual firmware consumer, `0x804c3ef4`. Each row is an eight-byte header
`<signed16 minimum temperature, signed16 maximum temperature, uint32 count>`,
followed by `count` pairs of `<uint16 voltage threshold, uint16 current>`.

| Temperature range (°C) | ≤ 4000 mV | ≤ 4100 mV | ≤ 65535 mV |
| --- | --- | --- | --- |
| 10–16 | 2520 mA | 2240 mA | 1320 mA |
| 15–21 | 2520 mA | 2520 mA | 1320 mA |
| 20–35 | 2520 mA | 2520 mA | 2240 mA |
| 34–45 | 2520 mA | 2520 mA | 2240 mA |

These are **table ceilings**, subsequently bounded by `charge-limits`, USB
source budget and other charger policy. They are not permission to request
2520 mA from a USB host, measured battery currents or documented safe operating
limits for arbitrary modified firmware.

The field identity is established by code, rather than the numerical pattern:
the battery updater stores property `Voltage` at charger-object offset 0x22c
and the `Temperature` property at 0x230. The latter's interned symbol is set
at `0x804c5d22`, using the `Temperature` string referenced by `0x804c5d16`.
`0x804c4d86` converts a signed 16.16 temperature by multiplying by 100 and
shifting 16 bits, confirming that row thresholds are hundredths of Celsius.
The nearby `NominalChargeCapacity` string does **not** identify the row selector;
that was an initial hypothesis disproved by tracing the actual consumer.

Selection retains a cached row while its inclusive temperature range still
matches. A fresh selection uses the first matching row. This explains the
overlaps: for example 20°C can retain the 20–35°C row when cooling from 22°C,
but initially selects 15–21°C. Cold-start values below 10°C or above 45°C
produce zero in the isolated table function. Its existing inhibition flags
also return zero; their complete origin has not been traced.

The `min-charge-capable-current` threshold of 1000 remains an observed policy
parameter, not proof of the current available from the Firebat USB port.
Its actual consumer compares accumulated per-port metadata and updates flags,
not a direct D1946 hardware charging minimum. See the
[consumer scope correction](p105-charge-guard-ownership.md).

The ADT also identifies a **Tristar** accessory controller at I2C address
`0x1a`, with compatibility strings `tristar,ths7383` and `tristar,cbtl1608`.
Its charger-identification path uses PMU ADC function `adcV` with channel 6
and the USB PHY's `DMON` function. This is concrete evidence that source and
accessory detection span more than the battery gauge. It does not establish a
VBUS regulator or permit blindly driving the Lightning port as a power source.

## Concrete D1946 firmware functions

The [17 October checkpoint](p105-charging-checkpoint-20261017.md) records the
completed USB500 discharge observation, improved REA function discovery and
the source VBUS getter's tested error contract. It supplies no new physical
charging or analog-retention qualification.

The D1946 kext is loaded at `0x80ecb000`. The power-source constructor at
`0x80ed0500` stores vptr **0x80ed3c68**, including the C++ vtable's 8-byte header
adjustment. Specifically, literal `0x374a` at `0x80ed0528` plus the add-PC
instruction's PC (`0x80ed0516`) plus 8 gives that address. Offsets in this
section are relative to the stored vptr, not to the raw table prefix.
Its base-constructor stub `0x80ed1f7c` resolves to `0x804c584c`, whose class is
`AppleARMFunctionCharger`. Thus the D1946 source directly inherits the generic
charger-function interface; an intermediate wrapper has not been established.

| Function | What the inspected code does |
| --- | --- |
| `0x80ed0558` | Initialises the source, reads register 0x3f low six bits as a current-code ceiling, and copies the first five `ichg-tbat-max` bytes if provided. |
| `0x80ed0768`, vptr+0x70 | Uses argument r2 as a current request; reads 0x3f and programs the five temperature-current slots at 0x4b/0x4d/0x4f/0x51/0x53. |
| `0x80ed0828` | Converts a requested current into a capped 40-unit code. |
| `0x80ed084c` | Converts the cached low-six-bit code into units of 40. This is not a battery-current measurement. |
| `0x80ed086c`, vptr+0x60 | Uses argument r1 as an input-current request, applies calibration/fallback conversion and writes register 0x3d. |
| `0x80ed08c8` | Converts input current to its register code, including a below-75 mode and input calibration/fallback compensation. |
| `0x80ed09d4` | Updates bit 0x04 of register 0x1b and bit 0x80 of register 0x1c together according to the below-75 mode. |
| `0x80ed0a98` | Decodes an input code as `75 + ceil(code * 100 / 8)`. Zero-request handling also requires the separate low-current state. |
| `0x80ed0c24`, vptr+0x5c | Reads six status bytes at 0x0a..0x0f; reports three booleans and the explicit reason `HighOrLowTemperature`. |
| `0x80ecef0c` | Handles `AppleRegisterDump` by reading PMU registers 0x00..0x7f and returning a data object. |

**Important distinctions from plausible but incorrect interpretations:**

- The current request does **not** simply replace register 0x3f's low six
  bits. Those bits are preserved. For a nonzero request, each thermal slot
  receives the minimum of requested-current code, cached per-slot maximum and
  the initial hardware-code ceiling.
- For p105 the first five thermal maximum bytes are `[0, 0, 63, 63, 63]`.
  The first two slots must remain zero. Their temperature ranges have not yet
  been established.
- A zero charge request sets 0x3f bit 0x40; nonzero requests clear it. The
  remaining bits are preserved by this method. This is an observed control
  behavior, not sufficient proof of complete charger enable/disable semantics.
- The PMU resume/event path sets register 0x1c bit 0x01 and clears 0x3f bit
  0x80. Their hardware meanings and relationship to charger initialization
  remain unresolved; the current-setter emulator deliberately does not claim
  to cover the initialization sequence.
- Raw D1946 setter functions do not fill the extra output pointer expected by
  one ARMPlatform helper. Direct inheritance is proven, but this argument's
  meaning and the apparent caller/backend discrepancy remain unresolved.
  It must not be translated into a guessed Linux API contract.
- Fault injection into the actual charge setter proves that a failed first
  read can be followed by register writes and a final success return. With the
  synthetic zeroed stack, unrelated control bits are lost. A Linux driver
  must preserve errors and abort before writes after failed prerequisite reads;
  this firmware behavior must not be copied as desirable error handling.

The ARMPlatform research independently found the policy/start helper at
`0x804c212c`, input/charge-limit logic at `0x804c437c`, and USB-stack current
reporting at `0x804c6fe0`. Together these show that **USB input current and
battery charge current are separate quantities**.

## Reproducible offline emulation

`tools/power/emulate-d1946.py` runs the actual Thumb instructions with Unicorn
2.1.4. The PMU object's read/write virtual methods are replaced by accesses to
a synthetic 256-byte array. Unknown calls are not silently treated as success.
The tool verifies the kernelcache SHA-256 before using any fixed address.

It produces JSON on stdout, including every fake read and write. In this
workspace the 39-scenario result is private
`../references/charging-research/d1946-emulation.json`. Synthetic starting
register values exercise bit preservation; they are **not live iPad values**.

Example observations:

| Request | Fake firmware result |
| --- | --- |
| Charge 500 mA | Last three thermal slots code 12 (480-unit setting), first two remain zero. |
| Charge 1000 mA | Last three thermal slots code 25. |
| Charge 2520 mA | Last three thermal slots code 63; larger requests clamp. |
| Charge 0 | Register 0x3f bit 0x40 set; no thermal slots rewritten. |
| Input 500 mA, no calibration | Input code 34, decoded 500 mA. |
| Input 1000 mA, no calibration | Input code 72, decoded 975 mA. |
| Input 1500 mA, no calibration | Input code 110, decoded 1450 mA. |
| Input 2100 mA, no calibration | Input code 158, decoded 2050 mA. |
| Input below 75 mA | Input code zero **and** the separate low-current state set. |

These numbers are firmware arithmetic under mocked conditions. They are not
measured charging rates, acceptable host power budgets, recommended settings,
or a complete simulation of the analog charger.

Twelve firmware regression tests cover temperature-table preservation, both
current ceilings, control-bit behavior, low-current mode, compensation and
rejection of the wrong kernelcache, including failed-read observations and
the actual status getter's output masks and USB/FireWire source criteria.
Run with the already prepared build image
or another environment containing Unicorn:

```sh
python3 tests/test-d1946-emulation.py
python3 tools/power/emulate-d1946.py build/firmware/kernelcache.12H321.macho
```

The tests explicitly skip when Unicorn or the locally extracted kernelcache
is unavailable. A skip is not evidence that firmware tests passed. No live
PMU transfer is supported by either program.

`tools/power/emulate-charge-policy.py` separately executes the actual
`0x804c3ef4` policy function with mocked OSData length/byte methods and a
synthetic charger object. It hashes the raw ADT as well as the kernelcache,
then reads the exact board table using Cascadia's existing ADT parser. It
does not emulate source negotiation, the complete charger state machine,
fault interrupts or the analog battery. The 70 cold-start cases and one
temperature sequence are private
`../references/charging-research/charge-policy-emulation.json`.

Its fourteen regression tests check negative/out-of-range temperature exclusion,
inclusive voltage thresholds and taper, first-match overlaps, cached-row
hysteresis, all four charge caps, inhibition flags, both firmware hashes and
the USB/adapter budget arithmetic described below:

```sh
python3 tests/test-charge-policy-emulation.py
python3 tools/power/emulate-charge-policy.py \
  build/firmware/kernelcache.12H321.macho \
  build/usb-host-research/firmware/DeviceTree.p105ap.raw
```

All twelve backend tests and all fourteen policy tests passed in the existing
resource-limited build image with Unicorn 2.1.4. The gauge production-C test
also passed with GCC undefined-behavior sanitizer. Four independently reverted
fixes each compiled and then failed their intended test assertion, confirming
that those tests detect the corrected behavior.

## USB source budget: enumeration and adapter classification differ

`0x804c75b8` uses source-description names from table `0x804d9940`:
`error`, `batt`, `unsupported`, `firewire`, `usb disabled`, `usb host`,
`usb device`, `usb brick`, `ac`, `external` for enums 0..9. This source
description is stored at byte offset 0x104 per port. The budget helper uses
a **separate** profile byte at offset 0x106. Numeric equality alone does not
prove identical enum types or the classification of an actual cable.

The actual `0x804c8014` helper, reached through `0x804c80e4`, applies:

| Profile at offset 0x106 | Budget calculation observed |
| --- | --- |
| 4 | 0 mA. |
| 5 | Uses USB stack type/report at 0x168/0x160. Type 3 gives 1500 mA; types 2/4 call the owner's policy method at vptr+0x350, which returns 1000 mA for AppleARMPMUPowerSource; other types use the stack-reported current. |
| 6 | 500 mA. |
| 7 | Adapter index at 0x178 maps 1..5 to 500, 1000, 1000, 2100, 2400 mA; unknown index gives 1000 mA. |
| Others | Returns an error without producing a valid budget. |

An optional override at 0x114 replaces the base value when byte 0x110 is set.
Wrapper `0x804c80e4` then applies signed offset 0x128, flooring subtraction at
zero, and applies the optional 0x120 cap when byte 0x11c is set. This explains
why the largest adapter-table value is not itself the board's allowed input.

The base instance constructor at `0x804c6108` stores vptr `0x804d9588`. Its slot
+0x350 resolves to `0x804c800c`, whose actual instructions unconditionally
return 1000 mA. Its slot +0x354 resolves to reset helper `0x804c8158`.
The actual `AppleARMPMUCharger` constructor `0x804c20c8` calls that base
constructor and stores vptr `0x804d8ce8`. It inherits the same 1000 mA method;
the input-budget emulator uses this derived vptr. It overrides +0x354 with
`0x804c3e24`, which invalidates cached source bookkeeping through `0x804c2a8c`
before forwarding to the base reset. This is not an observed PMU shutdown write.

The policy emulator executes both budget functions and the actual 1000 mA
policy callback. Every callback invocation is in its JSON output. The Python
class also supports an explicit synthetic policy override for experiments,
labelled `mocked`; neither the default CLI run nor the regression tests use it.
Tests distinguish a 100/500 mA USB report from adapter budgets,
the 1500 branch, unknown adapter fallback, invalid profile, signed adjustment,
override and cap. The actual source-detection producer is not emulated;
**no USB source is reclassified or granted additional current**.

Reset helper `0x804c8158`, when its reason argument is 1, stores 100 mA at
0x160 and clears USB stack type and adapter index at 0x168/0x178. It resets
additional state and converts profiles at least 2 to profile 4. The caller
and exact connection/suspend/detach event meaning still need verification;
this is not yet proof of a complete detach shutdown path. The helper makes
clear that carrying an old negotiated budget into a new connection would be
incorrect. No reset/classification callback was added to Linux.

The derived source-criteria method at +0x35c (`0x804c3e50`) aggregates each
D1946 backend's +0x54 getter (`0x80ed0b70`). That getter reads status registers
0x0a..0x0f and produces an OSSet of symbols:

- `USB` when register 0x0a bit 3 is set, and either the caller's qualifier is
  false or register 0x0a bit 6 is also set.
- `FireWire` when register 0x0a bit 4 is set. This is the firmware symbol's
  name, not proof of a physical FireWire interface on p105.

The symbol initializer `0x804c8558` establishes imported slots 0x804db00c
(`USB`) and 0x804db010 (`FireWire`), followed by `AC`, `External`,
`ExternalUnsupported`. Their pointers are mocked in the offline experiment;
the actual membership logic is executed with mocked OSSet allocation/add.
Tests distinguish all relevant bits and qualifier states, with only one fake
six-byte status read and no fake register writes.

The main source handler at `0x804c6880` obtains these criteria. An optional
`usb_det` function may supply USB-present state (stored object at 0xdc);
`0x804c6594` resolves this using the `function-usb_det` name. No such property
was found anywhere in the verified p105 ADT, so its alternative PMU criteria
path is relevant. When USB-present state is false, the handler at `0x804c6a26`
sets profile 1 and resets the cached budget with reason 1, except for port-state
0x142 value 4, which uses reason 0. This is concrete budget invalidation,
while complete hardware detach shutdown still requires tracing the later
input/charge setter application and accessory state transitions.

### Accessory power-handshake permission and refusal

The p105 `AppleTriStarBuiltIn` instance constructor `0x80885350` stores the
actual vptr `0x80888998` at `0x8088536e`. Its +0x36c entry
(`0x80888d04`) points to `0x8088586d`, the bridge to the previously resolved
`usb_dock_control` AppleARMFunction. The USB research traced the ID IRQ
through `0x80885160` and report stub `0x8088798c` to the
IOAccessoryManager consumer `0x807139b8`, which calls the power-handshake
routine `0x80714028` with the same owner. This proves the bridge for this
BuiltIn owner; other IOAccessoryManager classes must be checked separately.

Within that handshake routine, the path logged as skipping the handshake and
allowing charge calls +0x36c with flag `0x80000` at `0x80714260`.
The path logged as `power handshake verification failed` instead calls it
with `0x100000` at `0x80714cee`. These are software permission decisions,
not evidence of an analog PMU fault or successful physical charging.

The actual USBd consumer `0x804c70e4` handles the mask `0x1c0000` at
`0x804c7224`: port byte +0x130 becomes the complement of flag bit 19,
and +0x132 becomes flag bit 20. The whole-source budget function
`0x804c81a0` checks +0x130 at `0x804c8200` and forces that port's
contribution to zero when it is set (before subsequent source/minimum/maximum
policy processing). In particular:

| Input flag | +0x130 source inhibit | +0x132 handshake failure | Next call |
| --- | --- | --- | --- |
| `0x40000` | 1 | 0 | Source replan `0x804c75b8`. |
| `0x80000` | 0 | 0 | Debounce `0x804c7414`. |
| `0x100000` | 1 | 1 | Source replan `0x804c75b8`. |

`USBControl` in the policy emulator executes the actual USBd consumer and
derived/base reset instructions. Only cache bookkeeping, property publication,
debounce and whole-source replan are explicitly intercepted and recorded.
Tests prove these permission/refusal mutations, unchanged-classification
behavior, and invalidation of an old 2100 mA/adapter budget to a 100 mA USB
report with cleared source type/index and disabled profile. They do not
emulate a complete detach, authentication, PMU shutdown or fault lifecycle.
The CLI marks every intercepted call and never emits register writes.

## Status, faults and interrupt-consumption evidence

The actual PMU instance constructor `0x80ecb624` stores vptr `0x80ed3208`.
Its event-read method at vptr+0x388 is `0x80ecc460`, which reads **nine bytes
from registers 0x01..0x09**. The main interrupt handler `0x80ecbca4` consumes
those event bits and dispatches callbacks. No separate event-clear write was
found in the inspected handler. Treat this as an event-consumption range
with possible read-to-clear effects; absence of a public datasheet prevents
certifying the hardware clearing semantics from code alone. It must not be
included in an unsolicited diagnostic register dump.

Status is a separate read of 0x0a..0x0f via `0x80ecc450`. The actual source
status function `0x80ed0c24` maps:

| Output argument | Observed status condition |
| --- | --- |
| r1 boolean pointer | Any bit from mask `00 06 00 00 00 00`: register 0x0b bits 1 or 2. |
| r2 boolean pointer | Register 0x0b bit 3. |
| r3 boolean pointer | Register 0x0b bit 5. |
| Fourth pointer on stack | `HighOrLowTemperature` if register 0x0b bit 4; otherwise null. |

The first boolean contributes to aggregate `IsCharging` through the generic
source iterator; additional policy can also set that report. The generic
consumer associates the second and third with software log labels `END`
and `TO`, respectively; this does not prove hardware timer thresholds. See
[the status/recovery consumer experiments](p105-charge-status.md) for the
actual branches, boundary tests and explicit blocked backend calls. Generic
helper `0x804c5690` aggregates them, records a non-null reason at charger
offset 0x1c4 when empty, and conditionally invokes backend slots +0x74 or
+0x78. Those paths perform register writes; they were **not** emulated or
ported as active Linux callbacks. Slot +0x78 (`0x80ed0ce0`) selects the
low-current mode and signals the generic charger callback. Slot +0x74
(`0x80ed0c94`) reads 0x41, temporarily writes only its bit 3, then restores
the original byte. Its physical restart/termination meaning is not proven.

The offline backend tests execute the actual status getter with each bit of
synthetic register 0x0b, unrelated status bytes, and a failed transfer. The
failed read still returns success with absent outputs in this synthetic case;
copying that behavior could hide an actual overtemperature condition. A future
Linux implementation must reject unavailable fault status rather than assuming
it permits charging. None of these experiments measure battery protection or
assert that it is complete.

## Findings from other primary projects

### OpeniBoot

Inspected revision: `866562fdb1cfd019bcd77885c80fbf0af65d5c15`,
[iDroid-Project/openiBoot](https://github.com/iDroid-Project/openiBoot/tree/866562fdb1cfd019bcd77885c80fbf0af65d5c15).

The [S5L8900 PMU code](https://github.com/iDroid-Project/openiBoot/blob/866562fdb1cfd019bcd77885c80fbf0af65d5c15/plat-s5l8900/pmu.c)
distinguishes USB host, 500/1000 mA power adapters and USB suspend; it uses
GPIO-based charge settings. This supports the need for source-aware policy,
but those pins and register addresses do not describe p105. The
[S5L8720 implementation](https://github.com/iDroid-Project/openiBoot/blob/866562fdb1cfd019bcd77885c80fbf0af65d5c15/plat-s5l8720/pmu.c)
leaves charger settings unimplemented. No ready A5/D1946 charging driver was
found in this project.

### Sandcastle

Inspected revision: `0c2f7dda13d67bb7e06123df516f8bdb1000a79b`,
[Corellium/linux-sandcastle](https://github.com/corellium/linux-sandcastle/tree/0c2f7dda13d67bb7e06123df516f8bdb1000a79b).

The [PMU transport](https://github.com/corellium/linux-sandcastle/blob/0c2f7dda13d67bb7e06123df516f8bdb1000a79b/drivers/platform/hx/hx-pmu-i2c.c)
provides D2333 and Chestnut regmap profiles and GPIO/RTC/regulator children.
Its inspected Apple/HX PMU-related drivers contain no D1946 charging backend.
The generic structure is reusable, but copying its D2333 16-bit addressing
onto the p105 D1946 would be wrong; Cascadia already corrected this.

### Linux and Texas Instruments

Linux v6.12 `adc218676eef25575469234709c2d87185ca223a` provides the
[bq27xxx signed-current/status reference](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/power/supply/bq27xxx_battery.c).
The [power_supply documentation](https://docs.kernel.org/power/power_supply_class.html)
separates input-current limits from charge-current settings and defines the
sysfs units: microvolts, microamps, microamp-hours and tenths of Celsius.

[TI's bq27541 HDQ documentation](https://e2e.ti.com/cfs-file/__key/communityserver-discussions-components-files/196/bq27541_5F00_V200_5F00_DS.pdf)
describes a fuel gauge and communication protocol. Gauge configuration and
data-flash modification do not replace the missing D1946 charger policy and
are unnecessary for the current research.

## Safe tools prepared for later approved device work

- `tools/power/battery-snapshot.sh`: one sample of allowlisted Linux
  power_supply attributes, stdout only, no serial number, register probing,
  charger writes, SSH automation or persistent changes. Defaults to
  `/sys/class/power_supply/battery`. The optional directory argument supports
  offline fixtures. Missing attributes remain explicitly unavailable.
- `tools/power/pmu-diff.py`: strictly compares two **already saved** 128-byte
  AppleRegisterDump files offline. Reports changed bytes/bits and SHA-256
  provenance without producing write commands. Wrong-length input is refused.
- `tests/test-power-tools.py`: four offline fixture tests for allowlisting,
  preserved units, missing supply, exact bit differences and malformed dumps.

Reading arbitrary PMU registers is not automatically harmless: interrupt
status may clear on read. Therefore no blind Linux regmap dump, I2C scan or
`pmureg` command has been run. Existing upstream `pmureg` permits unrestricted
writes and must not be used as an automatic charger-enabling tool.

## Remaining work before enabling charging

1. Resolve the remaining caller/backend output-pointer discrepancy and trace
   the selected thermal/voltage table ceiling through the USB source-budget
   calculation into the D1946 setters. The table consumer itself is decoded
   and tested; direct backend inheritance is established.
2. Recover the charger initialization, charge-voltage target, temperature
   thresholds, protection/fault handling, restart/termination behavior and
   watchdog/interrupt requirements. Preserve the per-board thermal table.
3. Recover Tristar/PHY attach and adapter detection, Linux gadget-current
   notifications and suspend handling. Ordinary Firebat USB and Apple power
   adapters cannot be assumed to offer the same budget.
4. After explicit approval for the required device access, collect short
   matched iOS/Linux telemetry and controlled iOS register observations. Use
   the verified p105 board and firmware; capture source, battery temperature,
   state of charge and display load alongside each comparison.
5. Implement a separate D1946 charger power_supply driver with conservative
   limits, detach/fault shutdown and coordination with OTG VBUS sourcing.
   Keep experimental writes opt-in until short hardware tests demonstrate
   increasing charge, valid voltage/temperature and correct unplug behavior.

No device test, charging fix, live current measurement or battery-runtime
improvement is claimed. The useful completed results are the stronger gauge
driver, tested observation tools, exact firmware provenance and an offline
charging-control and thermal/voltage-policy experiments that can guide the
next porting step.

## Early P105 initialization follow-up — 14 October 2026

This separately mapped source is the original iPad2,5 iOS **6.0 / 10A406**
kernel component from the [official Apple restore archive](https://secure-appldnld.apple.com/iOS6/041-5525.20121101.Rstre/iPad2,5_6.0_10A406_Restore.ipsw).
Only the kernel member was fetched with checked HTTP ranges, ZIP CRC and
compressed-image bounds; the whole archive was not downloaded or hashed.
Community decryption-key provenance is the [exact P105 key page](https://theapplewiki.com/index.php?title=Keys:Sundance_10A406_(iPad2,5)&oldid=154515),
distinct from the official payload's provenance. AES/complzss length, Adler32
and Mach-O bounds were checked; the Apple Img3 signature was not independently
verified. Decrypted kernel SHA256:
`9b4acd2d171f8e39b1e047daf067ccfbdd7ce1b2382ddeb82247543e1adbd100`.

AppleD1946PMU1.0.1 is loaded at `0x80c8b000`, file offset `0xc4a000`,
size `0x15000`. It contains its own PMUPowerSource and does not declare the
later AppleDialogPMU dependency. All following addresses refer only to this
image; neither later addresses nor later ADT values are substituted.

**Startup writes are real, but do not prove complete charger setup.**
PMU start `0x80c8bc4c..0x80c8c1d8` initializes event masks and LDO suspend
defaults, and contains read/OR/write sequences for `0x9f` and `0x1c`.
Five original-instruction controls over `0x80c8bf96..0x80c8bfe4`, with fake
transport and seeded scratch RAM, cover success and each of the four transfer
failure positions. Each failure still permits all four operations; the next
instruction overwrites the status. This proves local failure continuation,
not the whole function's eventual success or a real fault on the iPad.
Transitive initialization callees are not globally proved free of analog
effects. These writes are not proposed for Linux replay.

**Two newly identified initializer reads are measurement trim.** Reads of
`0x55/0x56` produce a 12-bit owner field at `+0x94`; its actual consumer is
the ADC-channel8 branch at `0x80c8e0c6`. The independently resolved sensor-name
table identifies channel8 as `tjunc`. The software conversion accepts trim
1000..1100, otherwise uses1048, and yields Q16 temperature from
`(2600 - 33*(raw-trim))/100` with integer truncation. Thirty-five original
pure-arithmetic cases pass in fake RAM. This identifies junction-temperature
measurement, not a CV setting, autonomous thermal cutoff or physical
calibration.

**The battery profile estimates capacity.** The startup reads design capacity
and chooses a `battery-data` OSData profile using family/battery-ID, with a
`%04x-default` fallback. The selected byte pointer is stored at owner `+0x150`.
The consumer `0x80c94450..0x80c948a0` interpolates voltage-indexed Q16
percentage, scales by design capacity, stores a software history and can publish
`BootCapacityEstimate`. Nine original interpolation/helper executions across
the three authentic profiles pass with synthetic voltages, design capacity
and zero compensation. No PMU, ADC or property helper executes in that replay.
One profile has37 local voltage reversals; its original sequential search is
preserved. Neither voltage values in these profiles nor trailing unclassified
OSData are physical charging-voltage or protection thresholds. The selected
profile on the current live battery has not been established.

The source-start block `0x80c90b50..0x80c90bde` still reads inherited current
limits from `0x3f` and the five slot registers. Current-plan, low-input and EOC
paths independently match the later current-control scope. No CV target,
termination-current threshold or checked complete analog handoff has been
identified in these bounded paths. The five startup, 35 junction and nine
profile controls are private original-input source checks, separate from
ordinary public regression suites and all live hardware qualification.

No firmware bytes, profile tables, credentials or private test artifacts are
published. No new boot, PMU write or charger enable was performed for this
source follow-up. The running gauge/session and the inactive charger
experiments remain unchanged.

### Paired early boot stages

The matching 10A406 iBoot and LLB components were subsequently acquired from
the same official archive: only 430,171 compressed-member/header bytes, with
exact HTTP range/ETag, Deflate length and ZIP CRC checks. Both identify
iBoot1537.4.19; their native relocation entries independently establish their
different software bases. Decrypted component SHA256 values are:

- iBoot: `5e48cf1c3a6a666b9297e8c1d8cc06e7cf0a356e0e502c2a0002049ea900e9f3`.
- LLB: `c215fbe488f446691eac90205095bcff3f9f20aa04029188b84208638fe93b90`.

LLB startup call `0x34013d0e` reaches a real PMU initializer at
`0x3400aca0..0x3400af7c`: four table loops contain 10/36/14/8 records, alongside
a revision-conditional bank sequence and further control operations. These
match the previously examined later initialization family. Neither record-loop
callers nor the byte helper establish checked transport completion; the
initializer returns literal zero. Its opaque register/value records still
lack a named CV/termination/protection and reset-retention contract. A zero
initializer return therefore cannot qualify analog retention for Linux.

Seven specifically identified additional raw-write callsites address a
different I2C client (`0x34` encoded, versus D1946 `0x78`), with actual
source-classifier/accessory-response/precharge callers. One helper also invokes
the real PMU LDO-index path, traced to register `0x21`, mask `0x04`; these paths
are not generally PMU-free. They provide no new CV identity or electrical
power permission. This bounded caller closure does not exclude unknown dynamic
dispatch or other firmware paths. No further duplicate emulation suites or
hardware operations were performed. The remaining requirement is field
semantics and checked ownership of the identified initialization records, not
another numerical match to an opaque table.
