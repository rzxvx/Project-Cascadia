# D1946 ADC completion, IRQ and quiesce: offline evidence

This is a separate actual-code experiment for the normal ADC acquisition
path in the official p105 **12H321** kernelcache. All PMU transactions operate
on a fake byte array. It enables no Linux ADC callback, charger write or
device operation. ADC acquisition changes control state and is not a passive
hardware read. See [channel identities and scaling](p105-d1946-adc.md).

Pinned image SHA256:
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
Addresses below apply only to that image; no Apple executable bytes or
disassembly are included in the tool. Firmware provenance is recorded in
[charging research](p105-charging.md).

## Registration and actual interrupt dispatch

Manual startup inspection at **0x80ecb808..0x80ecb832** establishes the ADC
callback registration: packed event **0x120**, callback **0x80ecbe2d** (Thumb
entry), PMU owner, through registration helper **0x80ecbd90**. Event-table
entry **13** is then unmasked and its pending flag cleared. The actual PMU
vptr +0x370 method **0x80ecc674** clears bit 0x20 in its cached mask byte and
writes mask register **0x13**. This registration is inspected statically;
the bounded harness installs the same owner/callback/table fixture directly.

The harness executes main IRQ method **0x80ecbca4**. Its actual vptr +0x388
reader **0x80ecc460** obtains nine event bytes from **0x01..0x09**. Packed
0x120 therefore selects bit 0x20 in byte at register 0x02, index 13. For an
unmasked registered event, the actual dispatcher calls **0x80ecbe2c**.
That callback changes PMU software state +0x94 from **1 to 2**, then wakes
the command gate on the address of that state. In states 0, 2 or 3 it does
nothing and returns success. The callback itself performs no PMU I/O.

If entry +1 marks the event masked, the dispatcher marks entry +2 pending
and executes vptr +0x36c **0x80ecc608**, setting the cached mask bit and
writing register 0x13. It does not deliver the completion callback in that
case. The synthetic masked fixture accordingly times out. Event cache
merging, real IRQ electrical behavior and replay after unmasking remain
outside this experiment. Reading these event registers may clear pending
hardware events; the fake array does not model that physical side effect.

## Actual normal conversion lifecycle

The tool directly enters gated ADC method **0x80ecd9c8**, excluding its
diagnostic/BIST branch and the outer command-gate scheduler. State +0x94
must be zero before starting; the original waits otherwise.

Control byte **0x43** combines the selected channel (low five bits), mode
bit **0x40** when channel is 3 or the qualifier argument has bit 0 set, and
start bit **0x20**. The qualifier's physical meaning remains unknown. In the
mode-bit path the original first writes channel|0x40, then waits for a
deadline before writing channel|0x60. Otherwise it writes channel|0x20.

Deadline calls at **0x80ecda7a** and **0x80ecdada** use imported
`clock_interval_to_deadline`, respectively with **80** and **1000**, both
with scale **1000000**. These arguments encode **80 ms** settling and
**1000 ms** conversion waits, not 80/1000 microseconds. The clock routine's
target is resolved through import 0x80ed20cc / cell 0x80ed3108 to symbol
0x8001dc88. The time-unit conclusion is established by following that
target, rather than by the scale literal alone:

* **0x8001dc88** inlines the interval conversion, then adds the result of
  the current absolute-time helper at 0x800b2b28.
* Symbol **0x800b5adc**, `clock_interval_to_absolutetime_interval`, multiplies
  interval by scale as a 64-bit integer, divides by **1000000000**, and
  applies a common frequency variable via import cell **0x803920d4**,
  pointing to **0x803ab1b8**.
* Symbol **0x800b5a6c**, `nanoseconds_to_absolutetime`, uses the same divisor,
  imported frequency and quotient/remainder arithmetic on its 64-bit input.
  For `n = interval * scale`, both compute
  `q * F + floor(r * F / 1000000000)`, where `q = n / 1000000000` and
  `r = n % 1000000000`. Thus the scaled interval is in **nanoseconds**.

A private offline cross-check executed all three actual functions, including
their integer-division helpers, for 24 fixtures: four explicitly synthetic
frequencies and six intervals/scales including the ADC arguments, sub-tick
rounding and a 32-bit boundary. The conversion functions agreed, and the
deadline function produced synthetic current time plus the converted value.
This check used fake RAM and a mocked current-time helper; no frequency was
read from the device. These extra private experiments are distinct from the
ten repository lifecycle tests. The published acquisition harness's fake
clock merely records the deadline arguments; it does not measure timing or
validate the required analog settle period.

The clock comparison is now separately reproducible with a hash-pinned
tool and ten additional tests; see [ARM clock intervals](p105-clock-intervals.md).

Apple's historical [clock API declarations](https://github.com/apple-oss-distributions/xnu/blob/xnu-2782.40.9/osfmk/kern/clock.h)
also distinguish nanosecond conversion from absolute-time ticks, and
[clock_interval_to_deadline](https://github.com/apple-oss-distributions/xnu/blob/xnu-2782.40.9/osfmk/kern/clock.c#L835)
delegates to interval conversion before adding the current absolute time.
The pinned ARM image's own arithmetic supplies the unit evidence above;
the public revision is supporting API context.

The actual `IOCommandGate` vtable at 0x8039af00, address point +8, identifies
slots +0x7c (commandSleep), +0x80 (commandWakeup), and +0x84 (deadline
commandSleep). Apple's historical
[IOCommandGate header](https://github.com/apple-oss-distributions/xnu/blob/xnu-2782.40.9/iokit/IOKit/IOCommandGate.h)
documents the event/gate contract. That public revision is an ABI reference;
the tested iOS image reports xnu-2784.40.6, so the image's own symbols and
instructions determine the actual slots and behavior here.

After the conversion wait, state 2 permits the **two-byte read at 0x44**.
The stored sample is `(byte0 & 15) | (byte1 << 4)`. Channels 5 and 9 reject
byte0's sign bit with **0xe00002e8**; other tested channels ignore that bit.
If state is not 2, the method returns **0xe00002d6** without publishing a
sample. Cleanup writes control 0x43 with only its low five bits, sets state
to 0 and wakes waiting callers.

Synthetic wait results 2 and 3 cause another wait, not immediate cancellation.
An unrelated event also causes another wait. If the synthetic wait returns
1 while the actual IRQ callback has already set state 2, the sample is
accepted. This proves software ordering in the bounded experiment, not
real concurrent arrival, timeout latency or absence of stale interrupts.

## Original error handling must not become Linux policy

These are observed original-code behaviors, not recommendations:

| Injected failure in fake I/O | Actual result |
| --- | --- |
| Start write at 0x43 | Ignored; a separately injected completion can still yield success |
| IRQ read at 0x01 | Ignored by IRQ dispatcher; zeroed event-buffer fixture eventually times out |
| Raw sample read at 0x44 | Original I/O error propagates; output remains untouched |
| Cleanup write at 0x43 | Ignored; sample succeeds and state becomes 0 while fake control still contains start bit |

The IRQ-read case is fixture-specific: it does not establish the contents
of an actual failed or partly filled hardware buffer. A future Linux IIO
driver should propagate setup errors, treat cleanup failure as uncertain
hardware state, suppress invalid measurements and coordinate timeout and
late IRQ handling. It must not copy unchecked writes or infer cancellation
success from software state alone.

## PMU sleep waits rather than forcing cancellation

Actual active/sleep setter **0x80ecd7b0** waits on the same state address
while any conversion state is nonzero, then stores state **3**. With no
sleep-type property in the explicit fake root-domain fixture, cached sleep
type +0xe4 becomes **2**. This is not a forced ADC abort. The harness
rejects the indefinite busy wait instead of inventing a completion.

The opposite transition resets state to 0, cached sleep type to 0 and wakes
waiters. These bounded setter paths have no PMU transaction themselves;
they do not establish the surrounding physical sleep/resume sequence.
Linux cancellation during suspend therefore still needs a separately
verified control/IRQ protocol. No charger watchdog, target voltage or fault
shutdown is proved by this lifecycle.

## Reproduction and limits

```sh
python3 tools/power/emulate-d1946-adc-lifecycle.py build/firmware/kernelcache.12H321.macho
python3 tests/test-d1946-adc-lifecycle.py
```

Ten tests execute actual pinned Thumb code without skips in the existing
offline research image: sample boundaries; mode/deadline arguments; timeout,
unrelated and repeated waits; callback state guards; masked event dispatch;
completion-versus-timeout ordering; four I/O error locations; channels 5/9;
PMU sleep/wake state; and image/input guards.

Fake dependencies are PMU read/write, command-gate waits/wakes, deadline
conversion, event-mask lock/unlock, interrupt-state helper, logging, root
domain/property lookup and OSNumber metacast. The harness preserves actual
ADC method, main IRQ dispatcher, mask setter, event reader and completion
callback instructions. Each run has a bounded instruction count. The image
hash is required. No USB, I2C, device node, host networking or hardware API
is opened. Physical calibration, continuous acquisition, hardware event
clearing, pending-event replay and asynchronous scheduling remain untested.
