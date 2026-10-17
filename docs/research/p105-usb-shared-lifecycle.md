# P105 shared USB PHY lifecycle: consumers and failure limits

Historical diagnostic control: the subsequent [PHY reference-recovery patch](p105-usb-phy-reference-recovery.md) addresses the counted software path. These tests deliberately retain exact pinned original-source controls; their observed old failure is not a claim that the patched candidate still follows that path. Baseline bytes are read from a pristine tree or the verified local pinned Git object, never by undoing production edits.

Research date: 2026-10-11. Runtime review baseline: local `a812775`.
The runtime driver remains unchanged by this evidence group. Tests use actual
C functions with fake regulators, ordinary RAM, runtime-PM counters and mutex
stubs. No hardware register, USB device or iPad was accessed.

## Reproduced partial power-off / resume gap

The generic PHY core and the USB consumer must be considered together:

- Pinned Linux 6.12 [`phy_power_on()` and `phy_power_off()`](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/phy/phy-core.c)
  call the provider's power-on only at count zero, and power-off only at count
  one. A negative power-off return preserves that count and runtime-PM reference.
- [`usb_phy_roothub_power_off()`](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/core/phy.c)
  is void and discards each `phy_power_off()` error. Its suspend helper calls it
  before the wakeup-dependent exit decision. Resume calls power-on again.
- [`hcd_bus_suspend()` and `usb_remove_hcd()`](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/core/hcd.c)
  use these helpers. The ordinary non-auto bus suspend path also does not use
  the roothub suspend helper's return value. This is a concrete consumer path,
  not a hypothetical caller invented for the fixture.

The current S5L callback attempts both regulator releases and returns the first
error. With an early provider failure for LPO and successful HSIC release:

| Call | Core power count | Driver supply-reference flags | Result |
|---|---:|---|---|
| Initial power-on | 1 | LPO on, HSIC on | Success |
| Power-off, LPO release fails | 1 | LPO on, HSIC off | Error |
| Next power-on without an explicit retry | 2 | LPO on, HSIC off | Success; provider callback skipped |
| Balanced power-off | 1 | LPO on, HSIC off | Success; provider callback skipped |
| Final explicit power-off after fault clears | 0 | Both off | Cleanup succeeds |

The opposite early failure, HSIC retained but LPO released, exposes the same
count gap. There is no claim that either synthetic failure has occurred on our
device. A PMU error may also have changed physical state before returning;
the flags track software enable references, not measured voltage.

## This consequence was introduced by error propagation

The historical callback in upstream baseline
`a9bf28f20e5ce908fd5805e7d58b3ce9d9f0e4cf` ignored disable errors, cleared both
flags and returned zero. Consequently the generic core dropped its count to
zero, and the next power-on called the driver. Under the same early failure,
however, that callback lost track of the retained LPO reference and enabled it
again: the fake regulator records two references, with one still leaked after
the next balanced power-off. The old behavior did not have the skipped-resume
callback shown above, but it falsely reported cleanup and leaked a reference.

The later error-propagation correction fixed bookkeeping and exposed a new
resume failure: a successful subset of cleanup is incompatible with the
generic core retaining its all-on count. That correction must not be described
as complete suspend/resume recovery. The unresolved path is part of the local
PR review, even though normal activation and explicit cleanup tests pass.

## Candidate policies, not applied fixes

A release must preserve a usable state on failure, or the actual consumer must
handle a partially off provider before it attempts resume. Neither condition
is supplied by the current driver/core/HCD combination.

| Candidate | Advantage | Remaining problem |
|---|---|---|
| Roll back every successful disable when another fails | Attempts to restore the state associated with the retained core count | Rollback enable can itself fail; ordering, delays and hardware state need evidence. It cannot guarantee atomicity. |
| Retain supply flags but force a zero return | Core reaches zero; later power-on can enable only the missing supply | Reports successful power-off while a supply reference remains. Consumer teardown may never retry. Requires explicit retained-reference semantics; it is not an error-propagation fix. |
| Keep both WLAN supplies enabled across suspend | Avoids a partial supply release while the controller sleeps | Changes suspend power policy and requires complete lifetime/remove accounting. Higher power use and physical behavior remain untested. |
| Propagate/recover in the actual HCD consumer | Gives the caller an opportunity to prevent or recover a failed suspend | Requires wider USB-core/consumer changes and is outside a narrow S5L correction. Removal still needs its own policy. |

No global generic-PHY change, forced-success workaround, rollback register
sequence or changed WLAN supply policy was installed. The narrower next step
is to choose and document the S5L provider's intended suspend/remove semantics,
then test that contract through the actual generic-core and HCD lifecycle.

## Reproduction

`python3 tests/test-usb-phy-core-lifecycle.py -v` runs five passing unittest
tests without skips on this host. It extracts the current S5L supply helper
and power callbacks, plus both actual generic-PHY core power functions. The
core file must have SHA-256
`259e56ec9a9064cd120ab6f46e1457a5d78e089d56dbf989a0bb51297fe143f8`.
The default source path is `build/linux`; `CASCADIA_LINUX_SOURCE` may select
another local tree. Nothing is downloaded or executed from that tree beyond
the extracted, reviewed functions compiled with fake dependencies.

The cases cover balanced multiple-consumer sharing, both partial-release
permutations, explicit retry, the exact historical power-off callback, and a
temporary hidden-error control. The last control changes only the extracted
return to zero to demonstrate the tradeoff; it is never written into the
runtime driver. Each C fixture runs under UBSan and strict compiler warnings.
Mock mutexes do not establish concurrency safety, and early error injection
does not model the regulator core's coupled/parent/load rollback paths.

## Shared ownership still required for external Port1

P105 currently exposes two generic PHY instances backed by the same driver
object. Index 0 initializes the OTG gate and resets the shared PHY register
window. Index 1 manages the LPO/HSIC supply references, EHCI/OHCI0 gates and
the USB-complex HSIC-enable bit. Each generic PHY has its own core mutex and
count; those do not serialize operations across the two indices. This does
not alone prove a real concurrent reset bug, but it is no substitute for an
explicit shared-PHY ownership policy.

The existing [EHCI evidence](p105-usb-ehci.md) establishes that Apple retains
the same PHY provider for EHCI/OHCI, uses separate controller and removable
port clock-client masks, and calls PHY host-mode callbacks around inherited
controller transitions. The external type-2 Port1 is excluded from the HSIC
mask 4 used by WLAN Port3. Port detach must therefore not be treated as global
WLAN-supply shutdown or a shared USB20 reset. Complete inherited transition
error paths and arbitration still require tracing before an external route
can be implemented safely alongside WLAN. The independently built DWC2 host
profile has a different controller boundary and does not establish that
Apple's EHCI routing, tuning or power callbacks apply to DWC2.

## Configured exposure of the current bundles

Inspection on 2026-10-12 of both merged configurations (`build/linux/.config`
and `build/usb-host-research/kernel-output/.config`) finds `CONFIG_PM=y` but
`# CONFIG_SUSPEND is not set`; PM_SLEEP is absent. The diagnostics execute the
generic-core/USB consumer functions separately with fake dependencies. They
do not imply that system sleep is enabled or was exercised in these bundles.
The actual HCD bus suspend skips the roothub suspend call for automatic PM
messages. HCD removal still invokes the void power-off helper and discards
its error; retained references and later re-probe remain relevant. Thus this
is a concrete error-lifetime defect, not proof of a normal-boot failure or
a physical suspend incident on our device.
