# PHY power references: fail-closed recovery in the real USB consumer

Research date: 2026-10-12. This is a separate local kernel patch and offline
regression group. No iPad access, actual regulator/GPIO/MMIO operation, USB
transfer, boot or host configuration change was made. Kernel apply/build is a
separate integration step; passing these fixtures is not a hardware boot test.

## Concrete correction

`patches/tree/0002-phy-power-reference-recovery.patch` modifies five existing
Linux 6.12 files. The existing `apply-kernel-edits.sh` glob automatically
includes it; no build-script or global-PHY-count override is needed.

| File | Changed behavior |
|---|---|
| `drivers/phy/phy-core.c`, `include/linux/phy/phy.h` | Retain the first failed final provider power-off error under the existing PHY mutex. New power-on requests return that error until explicit final cleanup succeeds. Reject unowned power-off without a PM or regulator put. |
| `drivers/usb/core/phy.c`, `phy.h` | Track each roothub entry's acquired init/power reference, failed release and transaction-local unwind. Return power-off failures, recover retained references before acquiring new ones, and skip already owned references. |
| `drivers/usb/core/hcd.c` | Propagate PHY suspend errors. Perform pending recovery even on automatic resume before the controller's bus-resume callback. Report secondary add/remove/resume-unwind failures while preserving the primary error. |

The original successful generic-core count increments/decrements are retained.
The patch does not force a count to zero, call a provider callback outside the
core, invent register values, reset the shared PHY, or install the earlier
temporary supply-compensation candidate. Provider release/reacquisition uses
the existing operations and sequencing.

## Shared fault barrier and controlled recovery

A failed final provider power-off leaves `power_count == 1` and its existing
runtime-PM/top-level supply reference held, as before. The new error latch
blocks every consumer of that same generic PHY from adding another reference.
This closes the gap that per-roothub tracking alone could not cover.

Power-on checks the latch before acquiring new dependencies and rechecks under
the mutex after acquiring them. If power-off raced the dependency acquisition,
the existing error path releases only the new PM/top-level supply references.
Provider failures such as `-EBUSY` are conservatively blocked even when no
physical state change is known. The first error remains visible across later
failed cleanup attempts; only successful final cleanup clears it.

On roothub resume, all retained failed releases are cleaned before **any** new
power acquisition. Each retained entry gets at most one cleanup attempt in
that resume. A repeated error returns immediately; initialization/counts are
not duplicated and the HCD bus-resume callback is not reached. Successful
cleanup releases the old reference, then normal initialization/power-on
reacquires exactly the roothub's required references. Transaction unwinds
release only references acquired by that call, preserving earlier ownership
and recording failed cleanup.

The wrapper also keeps local initialization errors visible and does not exit
a PHY while it still owns a failed power reference. It does not add a generic
global initialization-error latch or claim to solve unrelated provider init/
exit partial-failure protocols. A separate diagnostic fixture confirms that
a foreign direct consumer can still raise retained generic `init_count` after
an exit error without re-running the provider init callback. The wrapper
blocks its own reuse but does not establish global init-fault safety. The
present S5L host PHY has no init/exit
callbacks; OTG's existing exit is a no-op.

## Additional concrete mode-unwind bug

The old `usb_phy_roothub_set_mode()` error unwind called `phy_power_off()` on
previous entries although mode selection had acquired no power reference.
`usb_add_hcd()` initializes PHYs and sets the mode before power-on. The exact
original-source control demonstrates count zero becoming minus one when a
later mode provider fails. The caller correction removes that release.
Another test runs the corrected caller with the original core, proving that
the caller fix is independent of the new defensive zero-count guard.

## HCD and upper USB-core boundaries

HCD suspend has already stopped the controller before PHY release. If release
fails, the patch reports the error while retaining actual
`HC_STATE_SUSPENDED`/`USB_STATE_SUSPENDED`; it does not pretend to restore a
running controller. This matters because `usb_resume_both()` dispatches the
device resume only for suspended/reset-resume devices. Automatic resume must
also honor the pending PHY recovery, instead of bypassing it.

Existing upper-layer `usb_suspend_both()` may return zero for an already
suspended root on a repeated suspend. That means already suspended, not
successful PHY cleanup or activation. The retained fault still blocks the
subsequent automatic and explicit HCD resume. Dead-HCD early-return semantics
are unchanged; no PHY power acquisition occurs in that path.

The HCD's existing lifecycle serializes its wrapper operations. The generic
PHY mutex protects the shared fault/count state across other consumers. Tests
mock mutexes and inject an off race at dependency acquisition; they do not
establish threaded lock-order correctness or asynchronous USB recovery. Normal
and experimental host configurations currently disable `CONFIG_SUSPEND`,
while `CONFIG_PM` and remove/reprobe/shared-consumer paths remain relevant.

## Verification and source provenance

The three new entry points pass **29 tests without skips** on the host and in
the existing network-disabled Ubuntu 22 build image: 27 compiled C fixtures
plus two baseline-source/provenance checks. All generated C runs with UBSan
and strict compiler warnings.

- `tests/test-usb-phy-fail-closed.py`: actual core power and PM helper code,
  actual S5L callbacks, shared consumers, early/late fault checks, dependency
  accounting, `-EBUSY`, PM-disabled behavior, unowned puts and historical control.
- `tests/test-usb-roothub-ownership.py`: real core init/exit/power functions and
  candidate roothub functions with circular fake lists; multiple PHYs,
  duplicate entries, independent roots, retained failures, one cleanup,
  transaction unwind and exact original mode controls.
- `tests/test-usb-hcd-phy-errors.py`: full actual HCD suspend/resume functions,
  with actual add/remove PHY-block excerpts. Unrelated bus/IRQ/DMA registration
  and teardown are not modeled.

`tests/usb_phy_candidate.py` verifies the five baseline file hashes, reconstructs
them in a temporary directory, check-applies and applies the actual 0002 patch,
then extracts candidate functions from that result. It never edits the working
kernel tree. Existing diagnostic controls now read pristine source bytes or
exact hash-verified local Git objects at pinned Linux
`adc218676eef25575469234709c2d87185ca223a`; archived Cascadia controls likewise
read verified local history. No handwritten baseline function copy or source
download is introduced. The old 11 diagnostic tests continue to pass.

`checkpatch.pl --no-tree --no-signoff` reports zero errors and zero warnings.
The patch carries no invented sign-off. Full kernel compilation and artifact
validation remain separate from these software fixtures.

## Explicit limits

Permanent provider failures remain fail-closed. `usb_remove_hcd()` is void;
it reports failed release/exit and does not falsify a put. It cannot guarantee
cleanup of retained references after its wrapper is destroyed. The patch
does not install background retry, an ownership transfer to a future reprobe,
forced regulator disable or automatic hardware reset.

The generic core's pre-existing error unwind can itself encounter a failing
top-level `regulator_disable()`. A fixture diagnoses a retained extra parent
reference in that late-race cleanup failure while the original fault remains
reported and power-count growth stays blocked. No general regulator dependency/
coupling/load rollback guarantee is claimed. The present P105 PHY uses its
driver-specific LPO/HSIC references, rather than a demonstrated parent-supply
recovery protocol.

Finally, releasing and reacquiring HSIC supply references may reset/disconnect
WLAN. Correct software counts do not prove firmware redownload, preserved USB
enumeration or physical WLAN continuity. External Lightning VBUS and route/
shared-PHY ownership remain independent hardware implementation blockers.

Primary code: pinned Linux
[`phy-core.c`](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/phy/phy-core.c),
[`USB phy.c`](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/core/phy.c),
[`hcd.c`](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/core/hcd.c),
[`driver.c`](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/core/driver.c)
and [`generic.c`](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/core/generic.c).
