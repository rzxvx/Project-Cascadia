# Actual end/timeout recovery owners, both source generations — offline original evidence

Original hash-pinned p105 kernelcache inputs:12H321 SHA256 `db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`;13G36 SHA256 `8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125`. Original top Mach-O segments map selected source methods; actual stored source-vtable slot contents are checked. All transactions use fake RAM/PMU bytes, not device transfers. No executable Apple bytes are embedded in tools.

The new group closes the previously blocked recovery bodies and a full original status-consumer→recovery chain. It does not identify physical CV/termination thresholds, autonomously safe defaults, or permit a hardware restart. The Linux current experiment already HOLDs END/TO/temperature (status0b38); no41 pulse is proposed or added.

## Exact ABI chain

| Role |12H321|13G36|
|---|---|---|
|Actual source vptr|80ed3c68|8114dbf0|
|Source PMU field|+18|+20|
|Pulse method/actual source slot|80ed0c94 /+74|8114a744 /+80|
|Input-mode RMW helper|80ed09d4|8114a3e4|
|Forced low-mode+notify method/slot|80ed0ce0 /+78|8114a794 /+84|
|Generic notifier import, explicitly mocked|80ed1f5c|8114be0c|
|Dictionary dispatch checked|No special-key path claimed|8056a3c0→8114a188→actual source+84|

PMU read/write virtual methods+378/+37c are replaced by explicit fake I/O while original source methods and dictionary-forward instructions execute. Source slots use instance vptr address points, not vtable headers. The13G36 dictionary checks presence of `AppleForceResetEndOfCharge` at string8114cb6c; any nonnull object triggers it. No OSBoolean value is evaluated. Logging and generic notification are mocked, not evidence of physical state.

## What the original bodies actually do

The pulse reads byte41, writes `original&08`, then writes the original byte. Both generations execute the same three transactions and return the **last** write's status. There is no wait, independent status confirmation or additional readback in this selected body. This is a finite control sequence, not a proven electrical reset/completed timer change. Firmware+74 is selected by the previously traced12H `TO` consumer, rather than by the `END` branch.

The input-mode helper reads two contiguous bytes1b/1c. Mode1 ORs first byte04 and second80; other modes clear those bits. It writes both bytes and returns that write result. Its read error is ignored. The forced outer+78/+84 methods call mode1, call notification with(source,0), and return0 **regardless of mode I/O result**. The13G36 force-EOC dictionary selects this mode path, not the41 pulse.

These are source-current/recovery operations. The mode helper is also used by the already verified below75 input handling. A debug key named end-of-charge does not supply a CV voltage or a cell-level termination algorithm, and its effect must not be conflated with the41 pulse.

## Executed transport counterexamples

Seven groups in `test-eoc-private.py` pass,0skip. For each source generation:

- Pulse preserves valid initial byte0/1/8/aa/ff with the exact temporary mask08.
- Failed initial read with fresh zeroed fake stack still writes0 twice, clobbering fake41=aa and reporting0. Actual failed-buffer contents remain unproven.
- Failed first pulse write is hidden by successful restore; failed last restore returns synthetic error `e00002c0` and leaves41=08.
- Valid mode1 preserves unrelated bits:1b=a1/1c=13→a5/93. Failed read writes04/80 and reports0; direct helper does return a failed write's status.
- Forced outer APIs, including the actual13G dictionary route, hide that failed-write status and still notify. They never write41 in these fixtures.
- Wrong original input and reuse reject before execution.

`eoc12-results.json` and `eoc13-results.json` record these original trajectories. Each emulator is fresh and single-use, capped10000 instructions; virtual PMU ranges are restricted to41/1byte or1b/2bytes. No hardware charging model exists.

## Newly complete actual12H caller→effect chain

`emulate-status-recovery-full-private.py` reuses the existing pinned `tools/power/emulate-charge-status.py` source/consumer fixture, but its two previously blocked methods now execute their original instructions. Only externalConnected, monotonic-clock sysctl and generic notification are mocked. Normal status snapshot and recovery PMU transactions all use fake bytes. It does not modify the public tool.

Three groups in `test-full-status-private.py` pass,0skip:

1. Status0b08 (`END`) at synthetic now401, capacity80/100, attached1 executes actual ARMPlatform consumer804c5690→source+78→80ed0ce0→mode09d4: read0a six bytes, read1b/1c, writea5/93, notify.
2. Status0b20 (`TO`) with first_timeout100 reaches strict `401>100+300` and dispatches source+74→80ed0c94: read41, write08, restoreaa. The consumer then clears reference+2c.
3. **A failed last41 restore leaves41=08, yet the actual consumer still clears first_timeout+2c to0.** A failed recovery read can similarly write41=0 and clear the timeout reference. Thus the caller's software retry state cannot certify completed restoration or physical recovery. This is stronger than inspecting the helper's returned error alone.

`full-status-results.json` preserves eight bounded original trajectories; `full-status-tests.log` records3PASS. The timestamp300 is the previously traced PMU counter/sysctl's software units, **not** the AIC microsecond clock used in the separate ADC6 boot group. No counter frequency, real cell capacity, electrical timing or live EOC is measured.

## Reproduction and scope

Private standalone source-method tests use `CASCADIA_TEST_KERNELCACHE`/`CASCADIA_TEST_KERNELCACHE13` or read-only container paths/fw12/kernelcache.12H321.macho and/fw13/kernelcache.13G36.macho. The full consumer experiment locates the repository through `CASCADIA_RESEARCH_REPO` (default/work), then uses its existing pinned12H tools/input. Both runs require already present Unicorn. Nothing downloads or writes firmware, and no normal source/config/build is changed.

This supports preserving the existing Linux error latch/HOLD policy and leaving debug recovery41/low-mode force off until independently qualified. It narrows actual owners/errors; physical CV target, termination, autonomous fault shutoff, OTP/boot-default provenance and retention across final iBSS/iBEC/Linux remain unresolved. No new guessed-CV C code, PMU driver action or retention=true assertion is introduced.

The root independently repeated7 original method groups and3 complete status/recovery groups; all passed with zero skip. Private logs `logs/night/eoc-source-root-repeat-v1.log` and `logs/night/eoc-full-status-root-repeat-v1.log`. The actual-C Linux worker continues to retain its fault on END/TO/temperature; none of these source operations is installed as automatic recovery.

Public optional reproduction (matching plaintexts obtained independently, Unicorn already available):

```sh
python3 experiments/p105-protection-evidence/check-eoc.py --kernelcache12 /path/to/kernelcache.12H321.macho --kernelcache13 /path/to/kernelcache.13G36.macho
python3 experiments/p105-protection-evidence/check-status-recovery.py --kernelcache12 /path/to/kernelcache.12H321.macho
```

These tools perform only original instructions in fake memory. They do not supply firmware or add normal build hooks.
