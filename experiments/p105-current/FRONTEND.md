# Isolated current frontend v6: actual owned source/gauge integration

This is an offline Linux C candidate. It is not installed, enabled, loaded, or
wired to a DT node. The Kconfig default and all four experiment parameters are
false. No device transaction or normal kernel build/configuration was changed.
It is not a physically validated charger and supplies no guessed CV, termination,
OTP, watchdog, temperature-threshold or analog-fault initialization.

The production current register core is byte-identical to v2. Its checked
transactions, current/profile readback interpretation, inherited limits,
exclusive read-clear event ownership, error latch and bounded shutdown attempts
are retained. The new code connects an actual producer and fixes lifetime races;
the further default-false USB2 selection policy below changes nominal plans
without adding analog/protection register settings.

## Actual producer attachment

`owned_usb2_source` is a read-only, default-false module parameter. When explicitly
selected in an independently reviewed experiment, `d_providers_acquire()` owns:

* the real battery supply reference from `apple,battery-supply`;
* the actual USB platform-device reference returned by `of_find_device_by_node()`
  for `apple,usb-source-controller` (with the OF-node reference put immediately);
* the actual combined provider handle from
  `p105_combined_provider_open(current_device, source_device, battery_supply)`.

The combined reader implementation lives in `../p105-source`.
It creates managed links to the actual controller and gauge platform suppliers,
pins their module/device/supply lifetimes, observes actual completed USB2
configuration budget and obtains an atomic bounded gauge sample. The frontend
owns the factory handle until after its work and IRQ users are drained.

The owned route has **no input power_supply lookup or dummy supply**. Worker
callbacks still pass its null input argument, and the actual combined producer
uses its owned DWC2 reader as the authoritative source. A source/descriptor
fixture cannot be substituted by platform-data permission in this route. The
legacy external-platform-data branch remains separate; its caller must supply
and own its existing provider contract.

Acquisition happens before any PMU read, with work/IRQs inactive. Every partial
acquisition is released by the same `d_release()` path. Failure to find a node,
platform device, battery, compiled provider or healthy reader produces an error;
there is no fallback to a fabricated current cap. The provider factory functions
are used only under `IS_REACHABLE(CONFIG_P105_COMBINED_CURRENT_PROVIDER)`.

No DT node/property was added to the normal system. The property names above are
candidate integration inputs, not assertions that the normal DT contains them.
The two original allow/retention assertions still deny probe by default. A
property write before `ready=true` now returns `-EAGAIN`, preventing the brief
power_supply-registration window from activating IRQs while probe is unfinished.

Neither route registers an `external_power_changed` callback into a foreign
supplier's workqueue. The owned route also registers a drained atomic USB-source notification; both
retain polling and their existing raw PMU source events. Cancellation of this supply's own changed_work
cannot drain an unrelated supplier's callback that already holds a class-device
reference; that reference also does not preserve this frontend's devm context.
The unsafe callback is removed, not merely guarded after entering freed context.
The same actual descriptor constructor is tested with both route selectors.

The owned observer now propagates USB suspend/reset/disconnect/configuration
invalidation directly to the frontend epoch and urgent work queue. Stop fences
queueing and drains this registered notifier before IRQ/work/handle teardown.
See [notifications](../p105-source/NOTIFICATIONS.md). This does **not** prove
immediate physical UDC-to-PMU shutdown or a USB electrical timing deadline. Validation rejects
observed source invalidation before/after every enable attempt; the existing
100 ms poll is not instantaneous physical cutoff. Removing the legacy callback
can change notification latency, which is explicit rather than called equivalent.

## Stop, IRQ and handle ownership

v2 had a concrete queue race: an invalidate/worker path could read `ready` and
`stopping`, pause, then call `mod_delayed_work()` after stop had completed
`cancel_delayed_work_sync()`. A device-link reference would not prevent that
late work from touching closed producer handles or freed consumer context.

v4's `d_queue()` holds `queue_lock` over both permission checks and scheduling.
All scheduling routes use this helper. `d_stop()` takes the same lock, marks
stopping, clears ready and advances the epoch. Thus scheduling finishes before
the stop fence or observes the fence and does not occur. The lock is released
before waiting for work or IRQ threads; stop never waits while holding it.

The actual release ordering is:

1. Serialize against property activation with `lifecycle_lock`; publish the
   queue/epoch fence under `queue_lock`.
2. Drain the owned combined-provider notifier in process context, outside
   spinlocks. Disable the activated, exclusively owned parent IRQ where necessary.
   `synchronize_irq()` drains **every registered** parent action, including the
   `parent_failed` path that used `disable_irq_nosync()` in its own thread.
3. `cancel_delayed_work_sync()` drains the worker. Future queue attempts are
   blocked by the fence. Under `io_lock`, perform one checked inhibit and one
   checked zero-input attempt if activation occurred. Failure retains
   `stop_error`/`fault_latched` and explicitly reports physical state unknown.
4. Unregister the exposed power_supply; property calls see stopped state.
   Remove/free the owned parent action before any nested mapping is disposed.
5. Free/drain nested child actions, dispose mappings, restore only owned mask
   bits with checked RMW, then remove the manual IRQ domain.
6. Close the combined provider after all callback/work/IRQ users are gone, then
   put the source platform-device and battery-supply references. This completes
   before driver-core devres/managed-link teardown can release supplier context.

The parent is requested without `IRQF_SHARED`, initially `IRQF_NO_AUTOEN`.
`parent_irq>=0` denotes a successfully requested action on all failure paths;
request/type failures reset it to -1. A parent thread therefore cannot look up
mappings in a removed domain or outlive the closed provider. The source raw
handlers cannot queue after stop; their `free_irq()` precedes context teardown.
This still requires the experiment to be the single PMU event owner, with no
independent ADC/watchdog/event-bank reader or foreign mask writer.

Suspend/freeze/poweroff use the same stop path. Resume does not resurrect a
retention assertion, old source lease or activation; a fresh independently
reviewed handoff/rebind remains necessary. Stop errors are not converted into a
successful analog shutdown claim. Remove cannot return that physical error, so
it retains the explicit diagnostic while completing lifetime cleanup.

## Bounded fault handling

The previous actual worker had another concrete failure: each negative supplier
snapshot called `d_fault()`, which unconditionally called `d_invalidate()` and
queued immediate work despite `fault_latched` already being true. Its final
no-retry guard could not cancel the earlier scheduling. Persistent EAGAIN/EIO
thus created a CPU/UART busy loop after the one allowed inhibit attempt.

Under the existing `io_lock`, v4 detects the first transition to the fault latch.
Every fault still clears applied validity and advances the epoch, but only the
first transition can schedule immediate work. A worker that causes its first
fault can leave one pending follow-up; the second failed iteration neither
queues again nor repeats PMU I/O because `inhibit_attempted` is already set.
The fault does not silently recover from later successful samples. New external
raw PMU events are distinct from self-requeue and remain subject to the stop
queue fence. No unknown status bit or recovery register is invented.

## Validation

Run these four self-contained entry points from the repository root:

* `python3 tests/test-experimental-current-lifecycle.py -v`: six GCC ASan/UBSan groups run
  actual candidate `d_providers_acquire/d_queue/d_invalidate/d_set/d_stop/d_release`
  functions against explicit kernel ownership/IRQ/work mocks. They exercise
  successful real-factory attachment, every partial acquisition error, absence
  of dummy supply lookup, unavailable compiled provider, exact reference unwind,
  source-parent IRQ removal before nested mappings and handle close, failed
  shutdown, parent_failed IRQ synchronization, the pre-probe write guard, and both descriptors lacking foreign callbacks.
  A deterministic pthread interleaving holds the actual queue call while stop
  begins; stop cannot pass the fence until scheduling finishes, then drain removes
  the scheduled item and 100 late invalidations cannot requeue it. This is a
  tested specific interleaving, not a universal concurrency-model proof.
* `python3 tests/test-experimental-current-pm.py -v`: one production-C PM group covers suspend,
  repeated stop, no-I/O resume, failed inhibit/zero-input and an inactive bind.
* `python3 tests/test-experimental-current-core.py -v`: two UBSan groups retain 16 original 12H321
  final-register oracle cases and checked I/O/readback boundaries. The oracle
  contains register results and an image hash, not Apple executable bytes.

The integration mock factory only returns a callback identity and can fail. Its
mock snapshot always returns `-EOPNOTSUPP`, validation always false. It never
fabricates positive source permission. Actual factory lifetime, HDQ transactions,
libcomposite set_config and completed-source grants are separately exercised by
the five actual-C groups in `../p105-source`.

* `python3 tests/test-experimental-current-fault-worker.py -v`: three ASan/UBSan groups run actual
  `d_fault()` and actual `d_worker()` against negative-only EAGAIN/EIO/ENODEV
  supplier callbacks and the production register core. Two worker iterations
  prove the immediate queue ends after one follow-up and I/O attempts remain
  bounded. Repeated fault calls still advance every epoch. A copied exact frozen
  v3 `d_fault()` body is compiled as a separate negative-control executable; it
  fails the same no-requeue assertion on the second worker iteration. The child
  core-dump limit is zero, and the historical code is never built into the module.

All twelve retained core/PM/lifecycle/fault tests and five source-notification
tests PASS. Missing GCC explicitly skips. ARMv7 object builds
against real Linux 6.12 headers with `W=1` PASS with no compiler warnings for both
reachable and unavailable provider branches (`arm-frontend-owned-v4.log` and
`arm-frontend-unavailable-v4.log`). The earlier v4 owned-branch `nm` shows its three combined-provider calls and
`synchronize_irq`; the unavailable branch has no combined symbols. The v5
notification group additionally ARM-compiles both branches and the actual
register/unregister calls; see its separate [validation](../p105-source/NOTIFICATIONS.md). `make prepare` explicitly refreshed the isolated generated
config before each branch: `olddefconfig` alone is not sufficient for an external
M= object build to consume fresh generated macros. The existing container UID
`whoami` diagnostic is unrelated to compiler warnings. No module was linked,
loaded or hardware tested. The normal `.config` SHA remains
`8b59b75e728cfacfb3441adf08b4de6e38c4ca53788fcee039ddddfdc43bed20`.

Primary kernel contracts:
[device links](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/Documentation/driver-api/device_link.rst),
[IRQ synchronization/free](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/kernel/irq/manage.c),
[workqueue cancellation](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/kernel/workqueue.c),
[power supply unregister/foreign changed work](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/power/supply/power_supply_core.c).

## Remaining runtime limits

The actual producer closes the former missing source/gauge factory gap. Current
CDC2 still provides 0 mA allowance. Explicit alternative USB2 descriptors can
supply at most 500 mA protocol allowance; they are not changed here. The frontend retains zero charge selection for owned USB2 by default. A further
default-false `allow_usb2_current` experiment can select bounded100/500mA plans;
see [its exact source, tests and limits](USB2_CURRENT.md). The legacy route
keeps its conservative experimental input>=1000mA restriction. This is
not proof that iOS cannot trickle-charge from 500 mA. No external charger/accessory
permission implementation is fabricated to bypass that limit.

Full CV/termination/fault protection and inherited analog retention, physical
source/suspend cutoff timing, complete event ownership and calibration remain
independent requirements before any runtime activation. The producer integration
and compile/test results are evidence for software ownership and transactions;
they do not prove electrical or battery safety or that charging works on hardware.

Public source stays outside the normal build. The source-provider include is
relative to the adjacent isolated component; no DT or enabled config is added.

The separate [CDC2 declared-demand candidate](../p105-gadget/EVIDENCE.md)
provides source-only default0/opt-in100/500mA descriptors. Actual composite and
completed EP0/source-reader tests close the former all-zero protocol-budget
gap for that candidate. The normal gadget remains unchanged; a USB declaration
does not replace analog protection, calibration or a wall-charger detector.

## Revocation during validator return — 14 October 2026

The worker checked its local epoch before calling the external source validator.
A source notifier could advance that epoch after the validator captured a true
result but before it returned. The unchanged-plan path could then publish an
obsolete plan; the pre-enable path could issue a stale enable, and the final
check could miss its intended inhibit. Recheck the frontend epoch **after** each
of these three validator calls. This closes the specified callback window; it
does not make bus writes atomic with subsequent source revocation.

The existing actual-C small-current fixture now exercises each return window.
The pre-enable case checks the write trace, since the old worker could enable
and subsequently inhibit, hiding the defect from a final-register-only test.
Three exact historical-worker controls fail the corresponding assertions; the
corrected worker passes. All 13 groups in `test-experimental-current-small.py`
and the four positive plus one historical control in
`test-experimental-current-inactive-zero.py` pass with GCC ASan/UBSan, under
1 CPU / 512 MiB without network or hardware access.

A separate private Linux 6.12 integration now completes a full ARM `vmlinux`
link and joint MODPOST/final link of `p105-combined-provider.ko` and
`apple-d1946-experimental.ko`. It includes both DWC2 patches, the corrected HDQ
snapshot supplier, the adjacent source/current directories and the CDC2
candidate. DWC2/libcomposite/HDQ and observer/source reader are builtin; the
combined provider and current controller are modules. All four runtime bools
remain false and CDC2 demand remains zero. The updated worker then passes an
incremental module link with zero warnings. Kernel W=1 warnings outside the
new charging components are retained in the private build logs; this is not a
warning-free whole-kernel claim.

This qualification uses an empty initramfs source and adds no P105 device tree,
probe binding or boot bundle. It does not change the normal build or demonstrate
physical charging. Analog regulation/termination/protection and PMU ownership
remain prerequisites for runtime activation.

## Bind the actual shared PMU transport — 14 October 2026

The previous probe cast arbitrary parent driver data to `apple_pmu_i2c` and
accepted its regmap. The current controller therefore did not establish that it
used the already prepared shared bank-zero transport. Both source routes now
obtain their map through `apple_pmu_d1946_bank0_get_for_child()` before allocation,
provider acquisition or the first PMU read. Without the experimental parent
configuration this gate returns `-EOPNOTSUPP`.

The accessor checks an actual platform child in probe, its I2C parent, the bound
PMU driver identity, D1946 match and P105 machine, then creates a managed supplier
link. It rechecks binding before interpreting parent driver data, verifies the
parent's registered regmap and matching bank-zero client, and rejects stopped,
sealed or faulted transport. These are memory/driver-core checks with no register
access. The link makes supplier unbind wait for child probe/remove; existing
worker/IRQ teardown still has to drain all users. A parent still probing defers
without taking the parent lock. Later state changes remain subject to the actual
regmap callback admission checks; the getter is not a lasting authorization.

`tests/test-experimental-parent-current-binding.py` executes the actual getter,
transport check, callback admission and both actual probe prefixes through the
last gate before allocation. Five positive groups cover identity, lifetime,
transport health, both routes and the disabled configuration. A SHA-pinned
historical probe prefix fails the new unbanked-map rejection contract with the
expected exit 23. Driver-core, OF and regmap lookup are explicit fakes; this does
not run a complete probe or test physical I2C. GCC ASan/UBSan and
`-Wall -Wextra -Werror` pass in a network-isolated 1 CPU / 512 MiB container.

The normal overlay/header, configuration and device tree remain unchanged.
This closes the specific unchecked parent attachment. It does not establish
exclusive PMU-event ownership, analog regulation/termination/protection,
retained configuration or adapter retry immutability. In particular the existing
`I2C_RETRIES` ioctl limitation in the [parent transport evidence](../p105-parent-pmu/EVIDENCE.md)
still applies. No charge-enable setting or hardware activation is added.

A subsequent private ARM Linux 6.12 candidate links this parent implementation
into `vmlinux` and both required charging modules in one MODPOST/final-link step.
The actual getter is exported by the kernel and referenced by the current
module. This incremental whole-kernel/module build completes with zero warnings;
the preceding candidate's unrelated full-rebuild warnings are unchanged history.
The ten existing parent transport regressions also pass. Compiled BSS verifies
`p105_bank0`, all four current bools and CDC demand default to zero. The private
kernel retains `I2C_CHARDEV=y`, so the documented retry-immutability limitation
is explicitly unresolved. The empty-source 512-byte initramfs contains only
`dev`, `dev/console` and `root`; this is not a usable boot package and was not
transferred or booted.

The next private profile now excludes builtin and modular `i2c-dev`; the parent
also rejects forced builds containing either. The known unlocked retry ioctl
entry point is closed in that profile, with real Kconfig n/y/m and initializer
regressions plus a complete ARM link. See the [parent exclusion evidence](../p105-parent-pmu/EVIDENCE.md#exclude-unlocked-adapter-retry-changes--14-october-2026).
This supersedes that specific private-profile limitation, not the analog or
physical-cutoff requirements. The running kernel remains unchanged.

## Revoke positive input plans before programming — 14 October 2026

The first snapshot validator can return its captured true result after a source
notification has already advanced the frontend epoch. The earlier worker then
inhibited charging but still programmed a positive input profile before its
later epoch check prevented charge enable. That later check was too late for
the input-policy writes themselves.

Keep the unconditional initial inhibit. Before starting a positive input plan,
check epoch, snapshot validity, epoch again and stop state. Known stale plans
exit without those input writes; zero-input programming remains allowed. An
inhibit failure still follows the existing fault path. This is admission before
a multi-register plan, not an atomic source lease across its transfers or proof
of instantaneous physical cutoff.

The existing actual-C small-current suite now passes 18 groups under ASan/UBSan
and Werror. New cases cover initial validator revocation, revocation during the
new input validator, a fresh positive plan, and actual positive-to-zero input
programming despite revocation. A pinned historical worker writes three positive
input registers after revocation in the counterexample and fails the new trace
assertion. Existing pre/post-charge validator tests use explicit old/new call
indices to continue testing the same semantic phases. These are register writes
in the fixture; no physical input current was measured.

The corrected ARM current module passes incremental MODPOST/final link with zero
warnings. The already qualified private kernel, combined module, configuration
and symbol exports remain byte-identical. Runtime bools remain false. Separately,
[disabled device-tree wiring](DT_WIRING.md) now describes the existing PMU IRQ,
battery and USB connections for an offline test candidate; it is not embedded
in this kernel or included by the normal build. No module or DT was transferred.


## Preserve an urgent event across periodic worker rearm

After successful publication and release of `io_lock`, a PMU parent event can
run `d_parent -> d_event -> d_invalidate`, advance the epoch and queue immediate
work. The previous worker then called `mod_delayed_work(...,100ms)` and moved
that already pending event response back to its polling deadline. The same
ordering can affect owned source notifications. This was a scheduling defect;
it did not establish an electrical cutoff deadline or new analog semantics.

`d_queue` retains its stop/ready/events/fault checks and `queue_lock`. Nonzero
periodic delays now use `queue_delayed_work`, which leaves pending work at its
existing deadline. Zero-delay urgent requests retain `mod_delayed_work`, so an
urgent event can still advance a queued poll. Linux6.12 clears a running work
item's PENDING bit before invoking its function, so an executing worker with no
pending follow-up can still queue its next regular poll.

`tests/test-experimental-current-urgent-queue.py` runs actual parent dispatch,
source invalidation and worker bodies with actual hash-pinned Linux6.12
`queue_delayed_work_on`/`mod_delayed_work_on` decision functions. The lower
PENDING/IRQ/timer machinery, time, PMU and supplier are explicit deterministic
fakes. It covers event-before-poll, poll-before-event, regular executing-worker
self-requeue, an earlier pending poll, stop/ready/events/fault gates, and parent
read failure through bounded inhibit. The exact old queue helper fails the
same event-before-poll check with next_delay100 instead of0. Six positive cases
and the historical negative control pass under ASan/UBSan/Werror. The five
existing extractors of `d_queue` have PENDING-preserving queue shims; their
retained assertions and old bug controls remain enabled. No actual workqueue
scheduling latency or physical charging behavior is measured.

Incremental ARM current-module MODPOST/final link passes with zero warnings.
The previously linked kernel, provider module, configuration and exports remain
byte-identical; all runtime opt-ins stay off. No device access, module load or
boot occurred. Run the focused test with `CASCADIA_TEST_LINUX` pointing to the
existing pinned Linux 6.12 tree; a missing compiler/tree is a skip, not a pass.


## Bypass supplier capture after a latched fault

A parent event-read error or IRQ-mask operation can latch a fault and queue
this worker before it starts. The worker previously requested a fresh supplier
snapshot before checking that latch, placing an unnecessary gauge/source
operation ahead of its existing inhibit attempt. It now checks the latch under
`io_lock` first and branches directly to the same bounded fault sequence.
The ordinary path releases the lock before calling the supplier and retains
its later locked check. No new register operations or retries are introduced.

The six actual-C fault-worker controls cover the first supplier failure,
prelatched inhibit/zero-input without supplier capture, repeated-run bounds and
inactive admission. The pinned previous worker fails the zero-snapshot check.
A separate old-worker/old-fault control preserves the earlier self-requeue
counterexample. The affected urgent-queue (7), small-current (18) and
inactive-zero (4 positive plus 1 historical) controls also pass with explicit
mock I/O, locks and scheduling. The initial small-current fixture still
expected a second capture; it was updated while retaining its historical
counterexample. These are software controls, not a measured shutdown deadline.

Both the current frontend and combined provider pass scoped ARM MODPOST/final
module links against a separate coherent HDQ-v2/USB-source kernel, with zero
warnings at one CPU/512 MiB. Existing kernel/configuration inputs are unchanged;
this is not a complete PR-head build or boot qualification. A fault arriving
inside an already running supplier call still waits for that call to return.
Analog initialization/retention and physical charging remain unqualified; all
activation gates stay unchanged.
