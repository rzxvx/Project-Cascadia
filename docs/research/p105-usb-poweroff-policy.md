# Temporary S5L power-off transaction: insufficient without consumer handling

Historical diagnostic control: the subsequent [PHY reference-recovery patch](p105-usb-phy-reference-recovery.md) addresses the counted software path. These tests deliberately retain exact pinned original-source controls; their observed old failure is not a claim that the patched candidate still follows that path. Baseline bytes are read from a pristine tree or the verified local pinned Git object, never by undoing production edits.

Research date: 2026-10-11. This experiment supplements the
[shared lifecycle diagnosis](p105-usb-shared-lifecycle.md). It does not alter
the runtime PHY driver. The candidate callback is compiled only in temporary
C fixtures with fake providers and register arrays.

## Candidate evaluated

The temporary callback attempts HSIC release first. If that release fails it
returns immediately, preserving LPO. If HSIC release succeeds but LPO release
fails, it attempts to reacquire exactly the HSIC reference released by this
call. Successful restoration sets the HSIC flag and performs the existing
ten-millisecond HSIC-on delay. Failure records a **fake fixture fatal counter**;
that counter is not a runtime diagnostic or deployed recovery mechanism.
The callback always preserves the original release error, including when
compensation fails with a different error.

This addresses one bookkeeping problem: successful compensation leaves both
driver supply-reference flags consistent with a retained generic-core count.
It does not make failed suspend/resume correct in the current USB consumer.

## Actual consumer result

The fixture runs the real Linux 6.12 `phy_power_on()`/`phy_power_off()` functions
and the real `usb_phy_roothub_power_on()`, `usb_phy_roothub_power_off()`,
`usb_phy_roothub_suspend()` and `usb_phy_roothub_resume()` functions. A one-entry
fake linked list models the current EHCI consumer's single PHY. Root-hub
init/exit are stubbed to record calls; they do not modify the core power count.

| Failure | Temporary supply outcome | Actual core/roothub outcome |
|---|---|---|
| HSIC release fails | Stop before LPO; both references remain | Power count stays 1; roothub resume returns success and raises it to 2 without calling the provider |
| HSIC release succeeds, LPO release fails, compensation succeeds | Both references restored; HSIC has been released and reacquired once | Suspend discards the original error and returns success; resume raises retained count 1 to 2 |
| Compensation also fails | LPO reference retained, HSIC absent; original error preserved | Resume still returns success at count 2 with HSIC absent; fixture marks fatal rollback |
| Both releases are injected to fail | First HSIC error stops before any LPO release attempt | First error preserved; no compensation occurs |
| All releases succeed | Both references released | Count reaches zero; resume calls the provider and reacquires both normally |

After successful compensation the supplies can be reference-consistent while
the caller's lifecycle is still wrong. A subsequent suspend only reduces
count 2 to 1 and does not call the provider. A subsequent resume raises it to
2 again. This leaves an unbalanced held core/runtime-PM reference and prevents
ordinary later suspends from attempting the supply release. Clearing the
original fault does not repair that count automatically.

For a non-wakeup controller, the actual suspend/resume helpers additionally
call exit/init; the power count remains retained and exhibits the same skipped
callback. The host PHY currently has no init/exit callbacks, so those calls do
not restore its supply state. This experiment does not execute the complete
HCD controller suspend or asynchronous USB-device recovery.

## WLAN and shared hardware are a separate boundary

A restored software regulator reference is not proof of uninterrupted WLAN.
Releasing and reacquiring the HSIC supply may reset or disconnect its device;
the fake provider does not model that physical effect. The ten-millisecond
delay is inherited from the current driver, not proof of sufficient WLAN boot
time or USB recovery. No verified chip reset, firmware redownload, HSIC bus
reinitialization, port-change interrupt or device-enumeration sequence was
added to the candidate. It also does not change EHCI/OHCI gates or Lightning
routing. Shared PHY reset/clock ownership remains an independent blocker for
external Port1 alongside active HSIC WLAN.

## Decision and verification

Do **not** install this candidate as a complete recovery fix. It improves a
subset of failed-release outcomes but leaves the actual consumer count gap,
and compensation itself can fail. A complete policy needs explicit handling
of failed suspend and retained references by the actual consumer, or a
documented different provider lifetime policy. No generic PHY core change or
forced-success runtime workaround has been made.

`python3 tests/test-usb-phy-off-policy.py -v` passes six unittest tests without
skips on this host under UBSan and strict compiler warnings. The harness
reuses the previous actual-driver/core extraction and pins the USB consumer
file SHA-256 to
`700f01a45cbfd4fa08778c1507ad352cc88ae11655437e0ffdad05a876efb7c6`.
Only the temporary power-off callback and fake enable-error provider are
substituted. Mutex, runtime-PM and early regulator-provider errors are
simulated; concurrency, late regulator-core errors and electrical state are
not established.

Primary code: pinned Linux 6.12
[`drivers/phy/phy-core.c`](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/phy/phy-core.c),
[`drivers/usb/core/phy.c`](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/usb/core/phy.c).
