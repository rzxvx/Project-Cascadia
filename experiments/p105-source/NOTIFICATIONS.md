# Private P105 owned USB-source invalidation notification

This source-only group replaces the owned USB source's previous 100ms-poll-only
consumer invalidation with an explicitly registered, synchronously drained atomic
notification path. All experiments remain default-disabled. No driver is loaded,
DT node added, PMU/PHY register accessed, descriptor altered, physical source
permission fabricated, or analog CV/termination/protection setting supplied.
The existing experimental >=1000mA charge-selection restriction is unchanged.
The owned source still grants at most500mA of completed USB2 protocol budget;
therefore this group alone does not make that route charge.

## Actual software chain

The existing Linux6.12 DWC2 hooks call `p105_udc_invalidate()` on controller reset,
disconnect, suspend, role invalidation and remove. The same observer now calls
its atomic chain after invalidating generation/state, and also when a new
SET_CONFIGURATION begins (or cancels an outstanding configuration), and when
both successful setup outcome and actual EP0 status completion commit a new
configuration. A draw request or setup success alone still grants no permission.
No DWC2 hook callsite, USB descriptor or hardware callback is altered by this
notification change.

`p105_source_changed()` revokes the combined tuple and increments its serial,
then calls the combined listener. `d_owned_source_changed()` advances the
frontend epoch and invokes actual `d_queue()`. The existing queue-lock fence
prevents work from being enqueued after Stop. The PMU transactions occur only
later in the existing worker/process context; none of these notification
callbacks reads HDQ, acquires an I/O mutex, or writes PMU/USB registers.
The existing worker rechecks its epoch/source lease before and after enabling;
this does not prove an instantaneous physical current cutoff during a bus write.

## Registration and teardown

All source and combined subscription changes are process-context,
caller-serialized constructor/remove operations. Each reader/provider owns at
most one listener, rejects a second with EBUSY, and holds managed device links
and existing module/device references. The link is to the matched actual DWC2
platform driver, not an arbitrary power_supply callback owner.

Frontend acquisition opens its existing actual source/gauge handles and
registers its notifier before PMU access or work/IRQ activation. Failure unwinds
through the existing release path. Stop marks stopping and clears ready under
queue_lock, then unregisters the combined listener outside spinlocks. Only
then does it synchronize its exclusively owned parent IRQ, cancel work, and make
the existing bounded inhibit/zero-input attempt. Failed inhibit remains latched
with physical state unknown. Idempotent Stop and no-reactivation Resume remain.

Combined close first unregisters any remaining consumer listener, then
unregisters its source notifier, before closing HDQ/source handles and freeing
its context. Source close also unregisters defensively. Unregistration must run
even after supplier/consumer link state becomes unbinding; testing 'currently
bound' before unregister would leave a live pointer to destroyed context.
Managed-link/devres ordering remains the existing reviewed supplier-lifetime
contract; an ordinary device reference alone does not establish drvdata lifetime.

Linux6.12 `kernel/notifier.c` actually removes an atomic notifier under its head
spinlock and calls `synchronize_rcu()` after releasing that lock (lines194..204).
Calls run within `rcu_read_lock()/unlock()` (lines225..234). Thus return from
unregister drains callbacks which could already have acquired the removed node.
The exact source hash is
`1f4a1b2384a8751d780d39a748ef5beda21149f6e715af54df131ff9add2c44e`,
Linux commit `adc218676eef25575469234709c2d87185ca223a`.

Lock direction is controller.lock (when held by the existing hook) ->
observer.lock -> source RCU callback -> combined.lock (released before calling
its listener) -> combined RCU callback -> frontend.queue_lock.
Callbacks must not capture a source/gauge snapshot, unregister themselves,
recursively access observer state or acquire lifecycle/io locks. Process-context
unregister holds no observer/controller/queue spinlock. The frontend may hold
its lifecycle mutex while draining because no notifier callback takes it.
The worker's source validator uses trylocks and does not hold combined.lock
while accessing controller/observer state.

## Tests and limits

Private tests compile the actual C source. Five retained composite/EP0/gauge
fixtures pass. Five new fixtures exercise all five controller revocation reasons,
configuration start/completion, a callback paused across Stop, source callback
paused across Combined close, supplier-unbind/error shutdown, and the historical
v4 Stop as a negative control. The latter fails the same assertion that work
cancellation must follow notifier removal. The child core dump limit is zero in
the portable test.

The threaded fixture compiles actual Linux notifier registration/call/unregister
function bodies and explicitly models RCU grace periods with pthread mutexes and
condition variables. Pausing wrappers deliberately model an interrupted atomic
callback; that artificial wait is not present in production callbacks. It is
not a proof of kernel scheduler timing, electrical cutoff latency or all possible
interleavings. GCC ASan/UBSan and deterministic barriers expose teardown order
rather than relying on sleeps. Gauge transport and permission semantics are
separately retained in the original five tests; no mock source grant is added.

The portable isolated export runs23 tests with no skips/failures, including
all current-core, fault-worker, lifecycle, PM, strict hook-apply and source tests.
Actual ARMv7 W=1 object builds cover observer, source reader, combined provider,
current frontend/core and full hooked DWC2 gadget/core/core_intr/platform/drd
translation units. The previous core_intr kernel-doc remotewakeup warning is
unchanged and was independently baseline-verified before this group. No module
was linked/loaded and no hardware behavior was measured.

Notification improves software event propagation only. A hardware/ADC/gauge
fault without a UDC event still needs the existing raw-event/status/freshness
and polling mechanisms. The analog retention assertion remains an explicitly
unverified experimental prerequisite, and no CV, termination, autonomous fault,
OTP-default, charger thermal-slot identity or adapter power calibration is proven.

## Public offline tests

Run `python3 tests/test-experimental-source-notify.py` and the existing
source-provider, hook-apply, current-core, lifecycle, PM and fault-worker tests
from the repository root. The source-reader patch retains its complete
observer-base-to-final observer diff, applied after observer-hooks. Neither
patch is applied by the normal build. Tests use the pinned Linux notifier and
composite sources from `build/linux`; missing prerequisites skip explicitly and
an existing wrong source hash fails. No new firmware dependency is introduced.
