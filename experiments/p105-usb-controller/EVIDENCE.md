# Denied-by-default P105 controller register primitives

Source-only proposal; no runtime registration,
DT overlay, role match, host publication or device access. Kconfig defaults n.

## Narrow implemented backend

`cascadia-controller.c/.h` uses actual Linux regmap/mutex/delay APIs to perform:

* USB-complex+0 selector bit0 leaf: read, clear bit0/write, read again,
  OR original already-decided host boolean/write. All other bits preserved.
  Original actual instruction block80b3a238..24e is exercised in the oracle;
  role decision/accessory detection preceding that block is outside scope.
* USB20 controller ARST pulse: the P105 property supplies native-LE index34
  (0x22); the original Performance reset action maps it to PMGR+1088,
  pulses saved-word bit31, delays1us, restores saved word. This is the EHCI
  controller reset and resets every EHCI port, including attached HSICWLAN.

Clock logical gate89's acknowledged register is also PMGR+1088 via its
separate firmware clock-table mapping. Logical clock ID89 and reset index34
are separate namespaces and must not be conflated.

The code requires caller-held **real shared owner mutex**, and explicit
verified contracts: correct P105 mapping/binding; complete existing clock
and PHY ownership; owned role lease bound to this exact epoch; owned gate89;
UDC quiesce; **all EHCI ports quiesced**; HSICWi-Fi quiesce; externally supplied
power/connector contract. These are owner attestations, not proofs magically
produced by booleans. The synthetic fixture sets them only for fake-RAM tests.
The current real Linux path provides none of this admission contract; normal
peripheral/HSIC/WLAN drivers remain unchanged, and the prototype refuses use.

Both gate target/ack nibbles must read0xf. Already asserted reset is rejected
EBUSY before writes; wrong/unfinished gate state gives EHOSTDOWN. Additional
entry guards are intentionally more conservative than original firmware.
Role lease epoch cannot be reused for a new connector epoch even if a fake
validator accepts it. Missing validator yields ENODEV; positive callback
errors normalize to EIO. This backend neither acquires/releases clock/PHY
references nor changes any supply, so it cannot fake missing parent closure.

## Error, epoch and lifecycle semantics

Every register read/write is guarded before and after its provider call.
Writes set `touched` before entering provider, since an error may follow a
physical side effect. Delay and final software completion have epoch checks.
I/O/late-epoch failure latches the original error; all later operations refuse
without I/O. No blind restore, reset retry or recovery is attempted.

A failed restoration may leave EHCI reset asserted. A failed host-OR may
leave mux bit0 cleared. Both are explicit partial states, not successful
host handoff. Their recovery needs a separately reviewed owner transaction.
Reinit returns EALREADY without erasing owned/touched/fault evidence.
Before any operation on a previously touched/completed context, an actual
validator failure latches the error even at admission; restoring the callback
result cannot reuse that context. A mismatched caller epoch is an argument
rejection before validator/I/O, and does not alter retained context. An
initial untouched validator rejection remains an ordinary admission error.
Completed-operation counter is diagnostics only; it is not complete role
state, clientstate5 or a controller-ready publication. A future shared owner
must serialize admission/publication and epoch invalidation; the callback's
snapshot checks do not themselves make external connector events atomic.

Use requires a sleepable role worker. Do not call from HCD `port_power` to
replace an I2C supply operation. Physical controller quiesce, ongoing WLAN
continuity, HCD suspend/reset ordering and adapter authentication remain open.
External powered Apple adapter feasibility is a separate researched hardware
path; its physical Linux behavior is still untested. No native5V source,
GPIO/PMU/Tristar value or charge-current operation is guessed here.

## Offline evidence and tests

Normalized `oracle-controller.json` SHA256
bf069ed8d7e3e917b676422206cffc81e9363bb33dc78d82cbfa668959abda43
binds original12H321 kernelcache and ADT hashes, eight exact mux controls,
eight ARST controls plus one high-bit-preservation pulse. Regeneration uses
only original bytes in the private pinned input and explicit fake IOKit/IRQ/
MMIO providers; JSON contains no binary firmware or private absolute paths.

`test-controller.py` compiles and executes the actual candidate C against
fake regmap and validator providers. Thirteen tests PASS, no skips:
actual oracle mux/reset words and delay; all certificate/default denials;
acknowledgement/asserted-reset guards; epoch/lease retention; every read/write
failure both before and after side effects; every I/O epoch invalidation;
every validation check including final commit; latched partial reset/mux;
reinit/null-and-ERR_PTR resource misuse; actual epoch invalidation after
completed operation with no subsequent I/O. These public-style tests consume normalized JSON
without importing a firmware path or installing Unicorn.

The C object also compiles against actual Linux6.12 ARM headers using the
existing ARM crosscompiler with Wall/Wextra/Werror: PASS, zero warnings.
This is an object check, not a complete linked driver or runtime hardware test.

Portable reproduction: `python3 tests/test-experimental-usb-controller.py`.
Only actual C with explicit fake Regmap/mutex/epoch providers is executed.
No controller/role/DT registration, native VBUS or first-boot claim is supplied.


## Retained controller readiness loss

Cold asserted-reset/unfinished-gate observations remain ordinary EBUSY/EHOSTDOWN
admission errors before writes. After a completed/touched controller operation,
the same observations now latch the original error and retain pending/completed
metadata. Merely restoring ready register bits cannot reuse the old context or
trigger another reset/mux operation. This closes a gap in the retained-owner
failure policy; it does not prove why real hardware lost readiness.

The preserved historical actual gate_ready function is SHA-pinned. Host controls
reproduce six prior same-context replay paths, then exercise six fixed retained
refusals and six cold admission/retry paths using actual complete controller C
and explicitly fake registers/owner/epoch providers. Sanitizers check host C only.
No physical USB, reset, power, WLAN, iPad or native kernel execution occurs.
Repeat with `python3 tests/test-usb-controller-readiness.py`. The source remains
uninstalled and denied by default; a separately reviewed owner transaction is
still required for recovery from a retained fault.
