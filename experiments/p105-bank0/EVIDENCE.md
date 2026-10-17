# Unregistered shared PMU bank0 transport — source-only candidate

`p105-bank0.c/.h` uses actual Linux I2C/atomic/mutex APIs and has no platform driver, Kconfig selection, DT, module init, normal MFD overlay, regmap replacement or activation setter. Fresh checked init sets enabled=false and **page0 unknown**. No production owner supplies a positive lease. No hardware write/read, event consumption, firmware or host configuration is performed by its tests.

Primary source: actual13G36 Dialog/D1946 bank descriptor/helper/read/write methods, original kernelcache SHA256 `8eae16f060a71d4f3790385951588f8198d0f21cdb74146f9750941a3d764125`, qualified in [the original bank audit](../../docs/research/p105-dialog-banks.md). Matching12H descriptor confirms01/03/57/1e/01; actual iBSS wrapper can hide a failed final1e00 selector. The new transport corrects **software routing/ownership**, not analog CV/protection initialization.

## One physical owner, checked selection and packets

There must be exactly one context for the physical PMU, with one shared lock/cache and supplier memory held across the entire call. **Every** PMU consumer, including charger, ADC, central IRQ/event owner, GPIO, suspend/shutdown and diagnostics, must participate. Per-child caches or independent raw/regmap/I2C accesses are prohibited by the provider contract. The component has no real provider registry/factory enforcing that contract yet: `exclusive_all_pmu` defaults false in an unsupplied lease and runtime remains disabled. It does not attach to existing MFD or silently make its accesses exclusive.

The matching PMU seven-bit address3c comes from p105 ADT. Valid resources and address are checked before I/O;10-bit clients reject. The first allowed call sends **[1e,00]** using a single `i2c_transfer` message. Selection cache becomes known only after exact message-count success and posttransfer lease/generation validation. No readable-selector semantics/readback is assumed. This is acknowledged software selection under the exclusive-owner contract, not a direct physical bank measurement.

Reads use one-byte low-register prefix plus a combined read message; writes use `[reg,data...]`. This is the ordinary Linux8-bit I2C shape (the pinned6.12 `drivers/base/regmap/regmap-i2c.c` supplies supporting API context). Sixteen independently normalized original-wrapper packet fixtures match these buffers. Unlike the original, any failed/short/epoch-stale transfer invalidates the cache, latches unavailable/fault, retains lease/error data, and requests global quarantine. A failed selector prevents data entirely. Partial failed reads are cleared; caller must honor any returned error.

`hold_unknown` runs while the owner lock is held: it may mark/enqueue quarantine, **must not reenter this transport or synchronously drain a thread waiting on its lock**. Caller shutdown must stop admitting new operations and drain holders before freeing adapter/client/ops/context. The component does not provide managed lifetime or a normal MFD teardown implementation. A hold-callback failure is retained separately; no physical stop guarantee is asserted after bus failure.

Explicit generation invalidation drops cached selection; a fresh successful read must select0 again. Generation wrap latches fault, preventing reuse of an old epoch. Reinitialization refuses initialized or dirty storage without overwriting lock, fault, lease or diagnostics. No automatic recovery/retry/clearing-unknown API exists.

## Intentionally narrow page0 interface

Only known experimental accesses are admitted:

- Read-only status0a..0f; read masks12..1a.
- Read/write current controls1b/1c,3d/3f; odd current slots4b/4d/4f/51/53.
- ADC control43 and read-only raw44/45.
- Central event owner only: read-to-clear01..09 and mask writes12..1a. A separate `event_owner` lease is required, independently of generic exclusive-PMU admission.

Extended bank1/2 data access, selector reads,41 recovery pulse,40/unknown controls, OTP/init table replay and arbitrary register ranges reject. Only selector0 is emitted, with the source-established1e write; no bank1/2 activation API is exported. This is not yet a drop-in full regmap bus: normal GPIO/RTC/poweroff users are not rewired. Current/gauge/source/ADC experiments remain unchanged and continue requiring their analog/lease/assertion contracts.

## Actual-C validation

Eleven complete-C groups PASS0skip in `tests/test-experimental-bank0.py` with host GCC Wall/Wextra/Werror plus ASan/UBSan. Entire source/header execute against explicit fake I2C messages, atomics, real pthread mutexes and synthetic owner leases. Positive fixtures are not real-source permission.

Tests cover disabled/unproved/ERR_PTR resources, unknown inherited bank2→checked selector0→status data, shared cache for successive callers, every selector/data fault and short result (including fake partial effects), cache invalidation/quarantine, lease/error retention and failed hold callback, generation changes/wrap/reselect, exact allowlist and exclusive-event restrictions, checked reinit, and **two actual callers** serialized through the shared owner lock. A paused first selector and second caller do not interleave; invalidation while paused makes the first stale and the second deny without further I/O.

Sixteen hash-pinned `native-page0-oracle.json` cases independently compare the C buffers/output against actual13G original read/write wrappers and selector. Software packet verification only, not a real adapter/PMU test. Original source has its own6+6 executable verification and last-iBSS-selector negative control.

Real ARMv7 object build against Linux6.12 commit `adc218676eef25575469234709c2d87185ca223a`, -Werror, PASS0warnings (private checkpoint ARM-object record). Only the existing isolated private Linux copy used; normal kernel source/config/build unchanged. It is object validation, not a linked active module.

This closes a concrete routing assumption in a source-only candidate. It **cannot set retained_analog_confirmed=true**: selector0/current/status readback does not prove CV/termination/autonomous fault settings, prior banked-init success, reset/suspend retention or calibrated physical measurement.

Root review additionally rejects positive validity codes, clears valid bounded read buffers even on argument/admission failure, and latches a failed owner validator after cached selection rather than resuming after its restoration. Invalid buffer/count cannot be cleared safely and returnsEINVAL. Read buffers must not alias source/lease/config memory. The added actual-C group covers these boundaries.

Root ownership hardening: the cache is bound to `owner_generation`. A different valid owner must reselect page0 even when the old software cache is set. A failed switch retains the new lease and latches UNKNOWN. Generations cannot be reused and every ownership gap still requires invalidation. The added actual-C control starts with an intentionally wrong Fake bank after a new owner and checks both successful reselection and failed-selector quarantine. Ten public test groups include the16 pinned original packet cases; no physical chip state is inferred.

Optional portable original reproduction: `python3 check-original-bank.py OFFICIAL_13G36_KERNELCACHE` checks16 packet fixtures plus native selector/cache/failure controls using caller-supplied hash-verified firmware and Unicorn. The independent root repeat passed; ordinary tests require no firmware.

Validator callback boundary: fault/generation is rechecked after the external owner validator returns. Invalidation inside the final ADC or bank validator now returnsESTALE with empty output and retained/quarantined state. Invalidation before a wall routing write prevents that transaction and still performs the existing checked release. Complete-C historical/fixed counterexamples were independently repeated. This closes the callback-spanning software gap; no atomic physical cutoff or shared supplier binding is claimed.
