# p105 charging status and recovery consumer: offline evidence

This research narrows the D1946 status bits and software recovery policy in
Apple's official iPad2,5 iOS 8.4.1 **12H321** kernelcache. It adds no charging
callback to Linux and performs no device access. Recovery methods are blocked
at their entry in the emulator, before any register transaction.

The source artifact and extraction provenance are recorded in
[p105-charging.md](p105-charging.md). Kernelcache SHA256:
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
The tool refuses other input hashes; no Apple executable bytes are included.
The addresses below apply only to that image, not the connected iPad's iOS
version or a different firmware build.

## What the actual consumer establishes

`AppleD1946PMUPowerSource` uses actual vptr `0x80ed3c68`.
Generic helper **0x804c5690** invokes source slot +0x5c
(**0x80ed0c24**) with three boolean outputs and a reason-string output.
The status getter reads six bytes from registers **0x0a..0x0f**, separately
from the event-consumption range **0x01..0x09**.

| Register 0x0b condition | Getter output | Actual consumer behavior |
| --- | --- | --- |
| Bit 1 or bit 2 | r1 boolean | Sets an aggregate boolean that contributes to published `IsCharging`; additional source policy can also set it. |
| Bit 3 | r2 boolean | Selects the log label `END` and can invoke source slot +0x78. |
| Bit 5 | r3 boolean | Selects the log label `TO` and can invoke source slot +0x74 after software delay. |
| Bit 4 | reason | Returns `HighOrLowTemperature`; stored at charger +0x1c4 if no prior reason exists. |

`END` and `TO` are **software labels**, resolved from PIC literals at
0x804c580c/0x804c5810 and instructions 0x804c5784/0x804c578a. They support
charge-end and timeout interpretations, but do not establish an analog fault
threshold, hardware timer duration or a complete protection mechanism.

Recovery processing requires `IOPMPowerSource::externalConnected()` to return
true and cached charger byte **+0x1c0** to be zero. The former is established
by import stub 0x804c9744 → 0x804d0118 → symbol 0x80302ad4. The latter is
populated from the `IsCharging` property in the charger property reader at
0x804c3038..0x804c305c. It is a cached software report, not a measurement of
battery current. The aggregate boolean follows a separate software path.

That path is statically connected to publication: caller **0x804c3960**
resolves PIC literal 0x804c3a30 at instruction 0x804c39c2 to the actual
consumer pointer **0x804c5691**, and passes it to the source iterator
**0x804c5618**. The result is accumulated on the caller's stack and stored
at charger **+0x221** (0x804c3a16). If the aggregate is still false,
additional source policy **0x804c7ab4** can set it from source classification
and its +0x130 state. Publication at **0x804c3542..0x804c3548** loads +0x221
and calls import stub **0x804c96d4** → pointer 0x804d00fc → symbol
**0x8030258c `IOPMPowerSource::setIsCharging(bool)`**. Thus the two status
bits contribute to `IsCharging`, but they are not the sole producer of that
property. This iterator/publication connection is manually checked static
evidence; the present tests execute the consumer, not its collection iterator
or property publisher.

The consumer requests a four-byte value using
`kernel_sysctlbyname("kern.monotonicclock", ...)`: PIC literal 0x804c5808
resolves to string 0x804cf3dc; stub 0x804c9ed4 → 0x804d02fc imports symbol
0x80278780. Failed sysctl requests or a returned zero block recovery.
It reads the sysctl without supplying a new value.

## Recovery branching and boundaries

For `TO`, source reference **+0x2c** records the first observed nonzero clock
value when it is initially zero. At capacity below **90%**, execution branches
directly to its retry check. If `now > first_timeout + 300`, it invokes slot
+0x74 and clears +0x2c. Equality is excluded.

This below-90% branch bypasses the later charger +0x1cd, attachment and
95% checks. That describes the original software; it is **not** permission to
bypass fault or current-budget checks in a future Linux driver.

For the remaining `TO` path, and for `END`, later conditions require:

- Charger byte **+0x1cd** zero; its producer is the `pmu-measured` property-presence policy, as traced in [guard-field ownership](p105-charge-guard-ownership.md). Its complete periodic lifecycle remains open.
- Current capacity strictly below **95%** of maximum.
- Nonzero timestamp **+0x1c8**, populated when `adapterInfo()` is nonzero. Its inferred attachment role does not make it a direct VBUS measurement.
- Current clock strictly below `attached_at + 54000` (0xd2f0).

`TO` then uses the same strict 300-unit check and slot +0x74. `END` invokes
slot +0x78 directly. If both bits are set, `TO` takes priority.
The actual code uses 32-bit arithmetic for these clocks and signed comparisons
of multiplied capacities; the harness bounds capacity fixtures to avoid
accidental arithmetic overflow. It does not replace the original wrap policy
with a modern Linux timeout policy.

The already inspected +0x74 method (**0x80ed0c94**) reads register 0x41,
writes a temporary value involving bit 3 and restores the original byte.
The +0x78 method (**0x80ed0ce0**) invokes a low-current operation and a generic
notification. **Neither method executes in this consumer harness.** Its output
records only which blocked method would be selected, not a safe hardware
restart sequence.

## Where the clock comes from

The D1946 RTC sysctl descriptor around **0x80ed4528** names `monotonicclock`
(string **0x80ed2c0c**) and points to handler **0x80ed1038**. That handler calls
**0x80ecd9b8**. Its underlying reader **0x80ecd970** reads four bytes at PMU
register **0x5d twice**, repeating until the two samples agree. After its
internal shift representation, the getter returns the same unsigned 32-bit
counter. The bounded clock experiment proves that reconstruction for selected
fixtures, including high-bit and all-one values.

The physical frequency, suspend progression and reset/persistence behavior
of that PMU counter have not been measured. The tests call 300 and 54000
**counter units**, not established charging-watchdog durations. Related RTC
code performs a conversion with multiplier 1000000, but this alone does not
certify the hardware timing. The clock reader also disregards I/O status and
has an unbounded stability loop in the original firmware; this behavior must
not be copied into a Linux callback.

## Reproduction and limitations

Run with locally available Unicorn 2.1.4 and the pinned local image:

```sh
python3 tools/power/emulate-charge-status.py build/firmware/kernelcache.12H321.macho
python3 tests/test-charge-status-emulation.py
```

Nine actual-code tests pass in the existing resource-limited Cascadia build
image, without skipped tests. They cover status bits/temperature reason,
external/cached-state/sysctl failure gates, first observation and strict
300-unit boundary, the 90% branch, 95%/54000-unit boundaries, simultaneous
`END`/`TO`, transfer failure, actual clock reconstruction and input guards.

The actual D1946 getter and ARMPlatform consumer execute. External-connected
reporting, the sysctl return and both recovery methods are explicit mocks.
The separate clock experiment executes the real stable-counter reader against
fake PMU bytes. All PMU transactions are emulator reads; no PMU writes occur.
Logging is disabled by a fixture, not used as an oracle for the control flow.
Restart method names are descriptions of previously inspected code, not a
claim that physical charging recovers.

The failed-status-read fixture demonstrates another gap: the original getter
ignores the transfer failure and reports absent outputs with this cleared
stack. Missing temperature/fault status must remain an error in Linux, not
be reclassified as healthy. Stack clearing makes these trials reproducible;
it does not establish the original firmware's behavior under all failures.

The separate [guard-owner trace](p105-charge-guard-ownership.md) resolves
the property and adapter-information producers of +0x1cd and +0x1c8.
Remaining work: emulate the broader publication and periodic policy; establish
actual thermal/voltage/termination protection; determine counter lifecycle; and design bounded fault-aware Linux state transitions.
No device read or charging-control experiment is authorized by this document.
