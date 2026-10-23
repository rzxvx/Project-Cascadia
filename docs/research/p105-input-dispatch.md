# p105 actual source-budget construction and charger dispatch

This separate offline group executes **0x804c81a0** and its source-budget
helpers, then intercepts the real derived charger method before it can access
the PMU. It tightens the software-owner chain preceding
[zero-input apply](p105-zero-input.md); physical source removal and a complete
charging shutdown are still unproved. No active Linux driver is added.

## Input and executed methods

Apple iPad2,5 iOS 8.4.1 / 12H321 kernelcache SHA256:
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
Provenance and board properties are in [charging research](p105-charging.md).
Public code contains no Apple instructions or firmware bytes; fixed addresses
require the user's local hash-verified input.

The tool reuses the existing actual input-budget harness. It executes:

- **0x804c81a0**, port-budget array construction and virtual dispatch;
- **0x804c80e4 / 0x804c8014**, actual profile/USB/adapter budget arithmetic;
- actual inherited policy method **0x804c800c** for relevant USB types.

The actual `AppleARMPMUCharger` constructor **0x804c20c8** stores address point
**0x804d8ce8**. Entry at **0x804d9040**, its slot **+0x358**, is
**0x804c437d**. The call at **0x804c8276..8280** reaches actual code
**0x804c437c** and is then blocked by the emulator hook. This demonstrates a
specific owner and method, rather than matching an unrelated class's slot.

One or two port descriptions/profiles/reports, global floor/cap and inhibit
fields are fixtures. Calibration/override/offset fields stay zero. A synthetic
stack canary replaces the external kernel value. The downstream current
planner is mocked at entry, without executing any PMU or USB operation. The
fixture's returned data has `hardware_access: false` and no transactions.

## Concrete budget policy before the dispatch

At **0x804c81fa..820c**, byte **+0x109** or per-port byte **+0x130+i** selects
an initial zero budget. Otherwise the actual source-budget helper is called.
If it returns an error, **0x804c81ee..81f0** substitutes **100 mA**.
Consequently unknown source profiles do not automatically become zero.

The next operations are ordered:

1. **0x804c820e..8218** raises the candidate to global signed floor **+0x138**
   when it is lower.
2. **0x804c821a..8224** lowers it to global signed cap **+0x14c** when higher.
3. **0x804c8226..8236** overrides the result with zero for description byte
   **+0x104+i** equal to **8 or 9** (`description & 0xfe == 8`).

The global/per-port inhibit path therefore does **not** by itself prove a final
zero request: its zero can be raised by the floor. Description 8/9 forces zero
*after* those clamps, but the physical producer and meaning of these enum
values remain unverified here.

Examples executed by the actual firmware:

| Synthetic classification / fields | Dispatched budget |
| --- | ---: |
| Profile 4, floor 0, cap 3262 | 0 mA |
| Profile 4, floor 100, cap 3262 | 100 mA |
| Invalid profile 0..3, floor 0 | 100 mA fallback |
| Inhibit set, floor 100 | 100 mA |
| Description 8 or 9, floor 100 | 0 mA |
| Adapter profile 7 / index 4, cap 500 | 500 mA |
| Floor 500, cap 100 | 100 mA, because cap follows floor |

These are policy arithmetic results, not permissible currents measured or
negotiated with a real adapter. Descriptions/profiles are distinct fields;
numeric equality does not assign a physical cable name.

At **0x804c8238..8244**, ports with description at least 3 may replace the
start index when their budget exceeds the currently selected port's budget.
The dispatch's flag at **0x804c826e** is the OR of `(profile > 1)` across ports.
It is not independently proved to authorize a physical charging operation.

## Downstream connection and remaining gaps

The derived planner's third argument is a **scalar start index**, not an index
array. In **0x804c4462..446a**, actual ARM code **0x804c92b0** calculates
`(start_index + iteration) % source_count`. Its ARM instructions use CLZ,
shifted subtraction and conditional moves; it must not be disassembled as a
Thumb API stub. This ordering observation is static evidence in this document;
the current group's tests stop at planner entry and do not claim an executed
remainder/PMU proof.

Within the derived planner, **0x804c45be** calls **0x804c2c68**. That method
iterates actual backend collection at **owner +0x184+i*4** and calls input
apply **0x804c479c** at **0x804c2cb4**. Separately, charge distribution calls
**0x804c46e4** → source slot +0x70 → D1946 **0x80ed0768**. These are statically
verified callsites; this group intercepts before executing their collection,
charge-ceiling, output-pointer or transaction behavior.

Source classifier **0x804c75b8** invokes **0x804c81a0** at **0x804c7832**.
Its later **0x804c829c(kind=2)** path queues worker **0x804c8390**, which calls
an owner **+0xc4** ARMFunction per port with argument zero. That later callback
is not evidence of a direct PMU shutdown. The direct planner transition occurs
earlier at 0x804c7832.

Still needed are exact detach event → description/profile/floor ownership,
source classification/workloop serialization, complete charge-current
reduction, hardware target voltage/termination and fault protection. This
proof explains why a Linux implementation must not simply map every unknown
or detached-looking enum to a guessed charger write.

## Reproduce

```sh
python3 tools/power/emulate-input-dispatch.py build/firmware/kernelcache.12H321.macho
python3 tests/test-input-dispatch-emulation.py
```

Ten actual-code tests pass with the local image and Unicorn 2.1.4. The tests
also verify two-port selection, clamp order, invalid inputs, a wrong hash and
single-use fixtures. Without the optional local image or Unicorn the tests
explicitly skip; a present wrong-hash image fails. `CASCADIA_TEST_KERNELCACHE`
selects an alternate local image path for the tests.
