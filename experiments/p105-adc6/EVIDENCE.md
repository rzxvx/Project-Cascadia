# Checked ADC6 polling candidate — source-only, unregistered

`p105-adc6-poll.c/.h` are an isolated real Linux-C object using regmap, mutex/atomic, `ktime_get_ns` and `usleep_range`. There is no platform driver, DT, normal build selection, PMU event/IRQ reader, module init, software current grant or charging activation. `p105_adc6_init()` admits only fresh zeroed storage and sets enabled=false. No production setter or owner factory enables it.

Original primary source is authenticated iPad2,5 13G36 iBoot SHA256 `7dc177231f52e7032cd10ddb255c3a8fd6f62c33c5e4d731eda7e5e0a8b8fdcd`, chainc88c and timer chain1ca98/1be4/1bfc/1c0c/1c1c, independently tested offline against the pinned original image. These source semantics support channel6 start43=26,43 busy20 polling, raw two-byte44 decoding and normal cleanup43=00. Software timeout50000 is50ms in the boot24MHz-reference contract; this is not a measured frequency or analog settling calibration.

## Ownership and checked behavior

The caller must hold regmap/provider memory lifetime throughout the synchronous operation and acquire an exclusive reservation for **all PMU ADC users**, not merely channel6. Supplied lease must separately prove no outstanding/late conversion. Both flags default false for an unprovided lease. No existing provider supplies these proofs, and no ADC IRQ-drain/cancel semantics are invented. Missing/unproved owner returnsEOPNOTSUPP before I/O. Context contains no upstream activation or reinitialization recovery route.

The checked candidate first requires43=00 while the lease is valid; a read alone cannot satisfy the quiescence assertion. It attempts43=26, waits1ms per poll, checks monotonic deadline (strictly>50ms), verifies control is only26 or06 and reads44 only after busy20 clears. A fast conversion can legitimately yield06 at the first read, so exact26 readback is not demanded. After raw decoding it rechecks deadline/generation.

Any attempted start is followed by checked43=00 write and checked43 readback, including start transport failure, stale source epoch, timeout or raw read error. Held resources permit this stop attempt even after authorization generation changes; caller stop/remove must drain this call before dropping backing memory. No event01..09 read, mask change, IRQ ack or presumed event drain occurs.

Every poststart error and uncertain initial control/read latches UNKNOWN, keeps the original lease identity and first/cleanup/hold errors, increments generation, and invokes the owner's `hold_unknown`. The owner must quarantine **all** ADC users. Failure of that callback is retained; this component cannot enforce exclusion in an absent/incorrect provider. Successful checked stop after a failed/timeout conversion does **not** clear UNKNOWN: late completion/cancellation remains unproven. Subsequent calls deny without I/O. Checked init returnsEALREADY without erasing initialized lock/fault/lease state; nonzero dirty uninitialized storage returnsEINVAL unchanged.

A successful output is only raw12-bit sample, generation and acquisition timestamps. It does not assert physical voltage calibration, VBUS capacity, safe analog charger initialization or permission to charge. A subsequent epoch change makes the observation stale; there is no instantaneous electrical cutoff guarantee. The source/classifier and gauge/current experiments are not wired to this candidate.

## Tests/build

Nine complete-actual-C tests pass with0skip in `tests/test-experimental-adc6.py`. Host GCC compiles the entire candidate/header under Wall/Wextra/Werror plus ASan/UBSan with explicit fake regmap, atomic/mutex, clock/sleep and reservation callbacks. Positive fixtures explicitly manufacture exclusivity/quiescence, not a real producer grant.

Coverage: default disabled; missing/unproved owner and ERR_PTR resources; raw0..4095 including high sign flag masking; exactly50ms completion; every transport transaction fault; simultaneous start+cleanup failure with retained error; cleanup mismatch; unexpected control;51ms timeout,64-poll frozen clock bound, clock regression, timeout during raw transport; generation changes at every I/O and after sample; preexisting busy control; hold callback failure; init refusal preserving unknown/lease fields. Every regmap transaction is constrained by the fake to43 or44, every write to43 value26/00. Failed samples leave the entire caller output zero.

ARMv7 object against actual Linux6.12 commit `adc218676eef25575469234709c2d87185ca223a`: W=1 PASS0warnings (`arm-object.log`), only the existing isolated private Linux copy used. No normal kernel source/config/build is changed. `test-results.log` records7PASS0skip. This is object validation, not a linked loadable driver or hardware test.

The separate original6-group harness demonstrates the original unchecked behaviors as negative controls and tests actual timer divide/inverse/current-counter instructions. It has no Apple code bytes or hardware calls. Matching official iBoot input is external/read-only; old12H offsets are not reused.

Generation wrap permanently latches UNKNOWN before any later capture. The public complete-C fixture includes a wrap/admission test. The original independent six-group instruction/timer verification is recorded privately without exporting firmware bytes.

Optional original-instruction reproduction requires the pinned official plaintext and Unicorn (no firmware supplied): `python3 experiments/p105-adc6/check-original.py --image /path/to/iBoot.13G36.dec`. Six original groups execute real tick division/inverse/counter instructions and normal/error ADC paths with fake I/O.

Heartbeat root review: provider int results are strictly0/negative errno. Positive values returnEPROTO. Wall acquisition with a positive result cannot establish a safely releasable lease: it retains supplied identity, latches fault and issues no I/O/release/retry. Validated acquisitions still perform checked release after malformed validate/settle/ADC results; malformed release retains uncertainty. ADC post-start validation errors still stop with held lifetime and retain UNKNOWN; malformed hold results are retained asEPROTO independently of the first transfer error. Eight actual-C groups now pass for each component;72 original wall comparisons remain unchanged. These are inactive source-only changes, with no normal build or RAM-bundle modification.

Validator callback boundary: fault/generation is rechecked after the external owner validator returns. Invalidation inside the final ADC or bank validator now returnsESTALE with empty output and retained/quarantined state. Invalidation before a wall routing write prevents that transaction and still performs the existing checked release. Complete-C historical/fixed counterexamples were independently repeated. This closes the callback-spanning software gap; no atomic physical cutoff or shared supplier binding is claimed.
