# D1946 source-current backoff, stored cap and deadline

Research date: 2026-10-11. Offline execution of the official iPad2,5
12H321 kernelcache, SHA-256
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
No hardware was accessed. This supplements `p105-source-events.md` and
provides reviewer-visible evidence, not a production charging implementation.

## Consumer and actual cap bookkeeping

Source callback `0x80ed06a4` can emit notification mask 6 when the cached
input code is at least 10, status 0x0b bit 0 is set and the live input code
is 2. The generic consumer is `0x804c2b08`. Its masks have distinct actions:

| Bit | Inspected consumer action |
| --- | --- |
| 1 | Calls debounce adapter `0x804c73e8` with argument 0 |
| 2 | Calls the same adapter with argument 1 |
| 4 | May compute a smaller input cap within the deadline below |

The consumer first applies `_OSBitAndAtomic` through stub `0x804c9424`
(import 0x804d0050, main-kernel symbol 0x802c3438). It tests owner
`IOService::getPowerState` through `0x804c9a94` (import 0x804d01ec,
symbol 0x802fa7c4); a zero return stops processing. These symbol names
are from this kernel's nlist, not inferred from numeric offsets.

Bit 4 proceeds only when the source's reference object +0x10 current is
at least 101 and `clock_get_uptime` is **strictly before** its 64-bit
+0x24/+0x28 deadline. Equality and later time skip the adjustment.
It invokes actual source virtual slot +0x6c with the previous current,
target 100 and step 100. For the constructor-proven D1946 vptr
0x80ed3c68, that method is `0x80ed0ab4`. The method also quantizes the
result to the input-code grid; it must not be replaced by an assumed
unconditional 100 mA subtraction with a 100 mA floor.

| Synthetic previous current | Actual method's candidate cap |
| --- | --- |
| 100 | Consumer skips adjustment |
| 101 or 199 | 0 |
| 200 or 275 | 100 |
| 300 | 200 |
| 501 | 400 |
| 999 | 888 |
| 1000 | 900 |
| 2100 | 2000 |

The actual cap helper `0x804c2bcc` stores changed candidates at reference
+0x18 and invokes the cap timer with interval 300 and scale 1000000000.
It does not reset that timer when the candidate equals the existing cap.
The harness executes the helper's branch and store; owner-name/logging and
timer delivery are mocked. No later source recalculation or PMU input setter
is executed by this experiment.

The zero candidates deserve caution: 101..199 are not forced to 100.
With a prior cap, the actual helper can clear it to zero. Input apply
`0x804c479c`, at `0x804c47c6..e0`, applies the stored cap only if it is
nonzero and lower than the effective budget. Therefore a candidate zero
cannot be described as proof that this path safely limits charging to zero.
Full policy propagation, reset timing and normal reachability of such previous
current values remain outside this harness. A new Linux implementation must
define its own valid lower-bound/error behavior before copying this policy.

## Producer of the 500 ms deadline

After a successful input-set path, the verified block
`0x804c4ade..0x804c4b10` stores current at reference +0x10 and change
at +0x14, saves current uptime at +0x1c/+0x20, and adds the converted
interval **500 × 1000000 nanoseconds** to that uptime. It stores the sum
at +0x24/+0x28. Thus the early source backoff test has a 500 ms window.
This is separate from the cap timer's interval 300, scale 1000000000;
those two intervals must not be conflated.

Clock helpers are identified by exact imports and main nlist symbols:

| ARMPlatform stub | Import | Main-kernel symbol |
| --- | --- | --- |
| 0x804c9c74 | 0x804d0264 | `_clock_get_uptime`, 0x8001dd1c |
| 0x804c9c84 | 0x804d0268 | `_clock_interval_to_absolutetime_interval`, 0x800b5adc |
| 0x804c9c94 | 0x804d026c | `_clock_interval_to_deadline`, 0x8001dc88 |

The isolated block experiment explicitly supplies the successful-set caller
context and synthetic return values from the first two clock helpers. It
executes the actual stores and 64-bit addition, then stops before following
caller-dependent timer actions. It does not execute or validate the preceding
input-setter result, hardware clock frequency or scheduling latency.

## Reproducibility and limits

```sh
python3 tools/power/emulate-source-backoff.py \
  build/firmware/kernelcache.12H321.macho
python3 tests/test-source-backoff-emulation.py
```

Eight actual-instruction tests passed in the existing resource-limited image
with Unicorn 2.1.4. They verify current/quantization boundaries, exact deadline
equality, the high 32-bit word comparison, inactive owner, independent bits,
changed versus unchanged cap/timer bookkeeping, 64-bit carry/wrap in the
isolated producer block, rejected input ranges and wrong firmware. Missing
local firmware or Unicorn explicitly skips the suite; that is not a pass.

Mocks cover clock conversion/uptime, the atomic pending-mask operation,
reported power state, debounce delivery, name/logging and cap timer delivery.
The actual consumer, source +0x6c method and cap helper execute. All current
and time fixtures are synthetic; no bus/device/SSH/network access is made.
The analog cause of status 0x0b bit 0 and register 0x3d falling to code 2
remains unknown. This is evidence for input-current adaptation under a source
condition, not proof of thermal shutdown, battery-presence detection, voltage
protection or permission to add a Linux charger restart callback.
