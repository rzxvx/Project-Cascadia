> Current input-handoff validation and transaction-order changes: [INPUT_HANDOFF.md](INPUT_HANDOFF.md). Historical implementation descriptions below predate this delta.

> The exported frontend now includes the owned source/gauge route and queue/IRQ/fault lifetime fixes. See [FRONTEND.md](FRONTEND.md); older staging paths below preserve original current-mask evidence.

> Isolated source experiment. Nothing selects this code in the normal kernel. Hardware behavior has not been tested. Local staging paths below identify the original evidence, not build hooks.

# Private Linux current-only / central-event candidate

Status: **offline source candidate**, no hardware validation, no device or host
configuration changes. These files are exported under `experiments/`; the normal build does not install them. No DT
node, patch-install hook or configuration selection is supplied. The Kconfig
option defaults to `n`; two read-only experiment parameters default to false.
No charger voltage, termination, OTP, watchdog or analog fault-initialization
value is guessed. This is not ready for a device trial.

## Concrete implementation

`apple-d1946-current-only.c` is an actual Linux platform driver using the
existing MFD parent's regmap, a threaded parent IRQ, a 72-entry nested irqdomain,
three internal source-event consumers, delayed work and a writable power_supply
frontend. It is not a Python permission simulator.

`d1946-current-core.c` contains the **same production C register transactions**
compiled by the offline fixture. Every read, write and relevant masked readback
is checked. Failed reads do not become zero or permission; failed writes do not
commit a software mask shadow. Partial writes remain possible and are explicitly
reported as an unknown physical state. I/O failure latches the frontend; no
successful later read silently clears this fault. Fault/removal paths each make
one bounded inhibit and zero-input attempt, checking each independently.

Only known current operations exist:

* `0x3f bit 0x40`: inhibit flag, remaining bits preserved. Inherited lower six
  bits supply the maximum code, not a guessed new analog configuration.
* `0x4b/4d/4f/51/53`: whole-byte thermal current slots. p105 maxima are
  `00 00 3f 3f 3f`; requested current is floored to 40mA codes and bounded by
  these limits and the inherited code. Bucket temperature meanings remain
  unknown, and no thresholds are written.
* `0x1b bit 0x04`, `0x1c bit 0x80`, `0x3d`: the explicitly traced input-limit
  path. Only six absent-calibration profiles (0/100/500/1000/1500/2100mA) are
  implemented. Nominal1000/1500/2100 profiles decode as975/1450/2050mA.
  Input reports derive from checked matching encoded readback, not a requested
  current or a physical current measurement.

Before a changed plan, charge is inhibited, the input profile is programmed,
then epoch and source/battery snapshot are revalidated before charge enable.
The epoch/snapshot is checked **again after enable**; invalidation causes a
checked inhibit. This cannot prove zero transient current between a physical
unplug event and its delivery to Linux. A fresh unchanged plan does not toggle
charging during the100ms status poll. Current and status reports are invalidated
by the generation change. Status has an additional maximum1s software age.

The frontend bounds an experiment to720mA charging and a2.8..4.1V,10..40°C
observation window. These are intentionally restrictive software policy bounds,
**not** recovered hardware CV/protection thresholds or calibrated measurements.
The p105 thermal table has10..45°C rows and current ceilings; its4000/4100mV
columns are current-policy thresholds, not CV targets. No board-data value is
presented as an absolute battery safety guarantee.

The 17 October correction narrows the frontend's previous 45°C limit to the
agreed 40°C experimental maximum. At 40.1°C the old worker admitted a positive
plan and retained an already applied plan. The corrected worker selects zero
input/current, requests inhibit and clears the input profile. The actual-C
small-current suite passes 30 tests with ASan/UBSan and Werror, including initial
admission, active-plan withdrawal, allowed 10/40°C endpoints and denied 9.9,
40.1, 45/45.1°C cases. Two pinned old-worker controls reproduce the 40.1°C
failure. A scoped ARM frontend object build passes W=1 without warnings.
These tests use fake PMU I/O and do not qualify physical thermal cutoff.

## Central ownership / initialization

Probe reads mask banks`0x12..0x1a` and the inherited`0x3f` control byte. It does
not read event bytes or write initial PMU masks. Child and parent IRQs start
with`IRQF_NO_AUTOEN`; supplier notifications cannot start PMU operations before
an explicit nonzero frontend request. Activation and removal are serialized.

The parent thread is the **exclusive reader of event banks`0x01..0x09`**. They
may consume pending events. No guessed ACK write is emitted. Cached masks commit
only after checked RMW/readback of the affected bits. Event mapping is
`bank=index>>3`, bit`1<<(index&7)`, mask register`0x12+bank`. Source consumers
are raw3,4,52 (bank0 bits08/10 and bank6 bit10); raw52 is not called a thermal
fault. Unclaimed events are consumed but not assigned guessed semantics.

Consequently this parent must become the single MFD event owner before any
other ADC/watchdog/status child is enabled: child consumers need their own
mapped event subscriptions. Running an independent ADC or other event reader
alongside it is invalid. `owned[]` records mask bits changed by consumers;
removal restores only those bits to their initial state while preserving all
others with checked RMW. This requires no concurrent foreign mask writer.

A supplied parent IRQ must already be explicitly level-low. Raw ADT matches
PMU interrupt-parent to GPIO-controller phandle, vector19/Apple flags1; original
GPIO init maps these to vector19 and level-low mode3. There is **no19→23
translation** in that evidence. No DTS interrupt property is added here.

## Missing producer / retention contract

The prototype refuses probe without `d1946_current_provider` platform data;
**no actual provider is included**. Its two callbacks must obtain and validate a
single coherent generation containing source permission, battery presence,
voltage and temperature. Snapshot validity includes monotonic sample/expiry
bounds (maximum1s). A nonblocking validation callback must immediately reject
source removal, suspend, role change, stale or invalid battery data. The worker
also holds real power_supply references through`power-supplies` (first input)
and a private`apple,battery-supply` phandle. The latter deliberately does not
misuse the standard`monitored-battery` battery-info property.

Implementing this producer is concrete remaining integration work: the current
S5L generic PHY and DWC2 UDC do not communicate a negotiated gadget-current
budget; gadget descriptors alone cannot grant it. Charger/accessory detection
also needs the traced Tristar/USBd path. Current gauge multi-property reads do
not establish coherent fresh snapshots. A DT assertion alone cannot implement
those missing runtime contracts.

The operator retention parameter is only an assertion. It does not prove
retention across the actual kDFU/iBSS/iBEC/kernel handoff. A later hardware plan
must document inherited CV/termination/temperature protection/watchdog behavior,
source removal and I2C fault response; determine non-destructive readback
coverage and which registers are readable; verify thermal/current units and
battery state under the actual board conditions. Those requirements remain
unresolved, so this code has no supported runtime activation instructions.

## Original-code evidence and test scope

Pinned12H321 kernelcache SHA256:
`db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.

Actual source instance vptr`0x80ed3c68`, source constructor`0x80ed0500`;
charge setter`0x80ed0768`, input setter`0x80ed086c`, status getter`0x80ed0c24`.
The p105 ADT has SHA256
`5c346586116de1bb32606acb27cad3158bcaa8a1318a10ee9631de09884cfd72`.
The source's five maxima come from`ichg-tbat-max` first five bytes. See public
`docs/research/p105-charging.md` and the unmodified
`tools/power/emulate-d1946.py` for the original-code oracle. Private13G36
comparison is in`../p105-private-13G36-charger-irq.md`; new offsets/ABIs are
not silently substituted into the12H321 comparison.

`oracle-private.py` invokes the existing pinned original Thumb setters in fake
RAM/PMU. It writes only derived transaction metadata and synthetic final PMU
states, not firmware instruction bytes. The16 cases cover charge
0/500/720/1000/2520 with synthetic inherited ceilings63 and24, and the six
input profiles. `test-core-private.py` compiles the actual core with gcc,
`-Wall -Wextra -Werror`, UBSan and error-stop assertions. All16 final256-byte
synthetic states match the original instructions. Transfer counts intentionally
differ because Linux checks readbacks and suppresses unchanged writes.
Additional actual-C fixtures cover each I/O failure position in the720mA and
2100mA transactions, mask failure/commit/preservation, mismatching readback,
unchanged inhibit and invalid ranges. Two unittest entrypoints PASS.

These tests do not emulate analog charging, battery safety, electrical IRQ
levels, a full Linux workqueue lifecycle or real power_supply producer callbacks.
The Linux wrapper still requires an actual6.12 ARM-object compile and root review.
No firmware, Apple instruction bytes, pairing material or device identifiers
are added to the candidate.

Reproduce core fixtures with`python3 test-core-private.py` from this directory.
Regenerate the oracle with the reviewed offline Cascadia image, network disabled,
upstream mounted read-only at`/work` and this private directory at`/stage`:
`python3 /stage/oracle-private.py`. The oracle requires the pinned local image;
it does not download firmware or access hardware.

Primary Linux API references (local verified Linux revision
`adc218676eef25575469234709c2d87185ca223a`):
[regmap interrupt implementation](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/base/regmap/regmap-irq.c),
[power supply interface](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/include/linux/power_supply.h),
[IRQ implementation](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/kernel/irq/manage.c).
Generic regmap-irq initially masks declared bits and does not propagate its
bus-sync mask-write errors to callers; this prototype instead retains initial
mask state until explicit activation and latches checked mask-write failures.

## v2 delta / sleep boundary

This is a separate private directory; v1 remains unchanged. The charge-current
API now rejects nonzero requests below40mA before any transaction or fault latch.
The same production validation function is tested for0/1/39/40/720/721mA.

System suspend/freeze/poweroff calls the actual stop function: first revoke
ready/epoch, disable the parent IRQ, drain work, then perform checked inhibit and
zero-input attempts and invalidate reported values. Any failed control attempt
returns its error to the PM core and latches failure. Repeated stop calls return
the saved result without repeating transactions. Resume/thaw/restore deliberately
restore no source lease or analog-retention assertion; this bind remains stopped.
A new reviewed handoff/rebind is required, including when a sleep attempt fails.
`DEFINE_SIMPLE_DEV_PM_OPS` covers both suspend and hibernation. The current
runtime configuration has CONFIG_SUSPEND disabled, but the implementation does
not silently assume retention if that changes.

`python3 test-pm-private.py` compiles the actual extracted stop/suspend/resume
functions together with the production transaction core against explicit fake
IRQ/work/mutex/regmap I/O. It verifies successful stop, both control-error
positions, repeat-call behavior, resume denial and an unactivated no-I/O stop.
This tests production C logic, not the complete kernel PM machinery.

Root compiled v1's two ARMv7 objects against the real Linux6.12 headers with
W=1 and no warnings. v2 needs its own ARM compile and review. Its private core
and PM fixtures currently PASS (three unittest entrypoints total).

The current-only prototype restricts enabling to nominal input profiles of at
least1000mA as an experimental policy. The traced owner+0x1d0 consumer compares
an accumulated value, not a directly proven USB-budget minimum. This restriction
is not evidence that500mA cannot physically trickle-charge the battery.

## Zero requests before event activation

A ready inactive frontend previously accepted zero-current/zero-input writes,
but its disabled queue never applied them. It now returns-EAGAIN before changing
requested values; it does not enable IRQs or add a PMU operation for that request.
Active zero remains asynchronous. Four actual setter/queue/worker groups plus
the retained old-setter assertion control cover this path; the independent
export also seeds prior nonzero requested values to verify preservation. The
actual current core is compiled into the fixture; source/IRQ/bus providers are
synthetic. This fixes an unapplied-success report and proves no physical inhibit,
charging or supplier permission. The experiment remains uninstalled/default-off.
