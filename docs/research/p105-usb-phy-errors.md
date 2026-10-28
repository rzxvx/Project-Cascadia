# P105 PHY: power and initialization error handling

Research date: 2026-10-11. This is a local source correction with mocked
callback tests. No USB device, iPad supply or PMGR register was accessed.

The existing `s5l_hostphy_power_on()` in
`patches/files/drivers/phy/phy-apple-s5l-usb.c` enabled LPO and HSIC supplies
before enabling the host power gates. Three failure paths were misleading:

- A deferred HSIC provider or failed later enable left a newly acquired LPO
  enable reference in place.
- An EHCI gate timeout left newly enabled supplies in place; an OHCI0 gate
  error was ignored entirely and the callback returned success.
- Power-off ignored failed regulator disables and cleared both state flags,
  losing track of failed cleanup.

The correction propagates acquisition errors other than optional-supply
absence (`-ENODEV`), propagates both gate errors, and undoes only supply
references acquired by the failing call. It preserves the original failure
even when cleanup also fails. Previously enabled references are retained.
Power-off attempts both releases and returns the first error; a failed release
retains the corresponding flag for a subsequent attempt.

There is no new PMGR gate-off/reset sequence. A failed PMGR enable might have
changed gate state before its timeout; the patch does not claim to restore it.
There are no new PHY tuning, Lightning routing, VBUS or charger writes. The
successful power-on sequence retains the previous register operations.

## Scope of reference accounting

The flags represent this driver's consumer enable references, not a measured
rail voltage. P105's `wlan_lpo` and `wlan_reg_on` supplies use the existing
`apple-pmu-pwrsw` provider, with enable/disable/is_enabled operations. The DT
has no parent supply, coupling or consumer load for these supplies. In that
topology a provider disable failure returns before the consumer reference is
released.

This is not a general guarantee of `regulator_disable()`. Linux v6.12
[`drivers/regulator/core.c`](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/regulator/core.c#L2983)
can also report an error after releasing a consumer reference, from load
updates, coupled-rail balancing or a parent-supply disable. Reference/retry
handling needs another review if the board's supply topology changes.

The Linux PHY core retains `power_count` after a failed power-off callback;
an explicit subsequent power-off can therefore retry cleanup. This does not
establish that a new power-on after partial cleanup restores both supplies:
the core skips the power-on callback while its count is nonzero. That shared
consumer lifecycle is reproduced in the separate [core-count and shared-lifecycle tests](p105-usb-shared-lifecycle.md); it remains an unresolved consequence of the error-return change. The test models early provider
failures only, not the regulator core's complete dependency graph.

## OTG initialization and optional resource mapping

`s5l_usbphy_init()` previously ignored `s5l_pmgr_on(PMGR_USB_OTG)`'s return
value. If the PMGR status did not acknowledge the gate request, the helper
returned `-ETIMEDOUT`, but initialization continued into delays and two writes
to the PHY reset register and returned success. Initialization now propagates
the gate error before those reset writes and post-gate delays. Pre-initialization
register reads and the original gate request/poll still occur; this is not an
MMIO-free error path or a gate rollback. The successful reset sequence is
unchanged, and a later acknowledged gate request can retry initialization.

The second MMIO resource remains optional when it is absent. When the resource
is present, however, `devm_platform_ioremap_resource()` can report a real
mapping/request error. Probe previously changed that error pointer to `NULL`
and published both PHY instances. It now returns the original error before
creating or publishing either PHY. A present resource that maps successfully
retains the previous behavior. No optional-resource requirement or host-role
policy was added.

## Verification

`tests/test-usb-phy-power.py` extracts the actual production callbacks and
gate constants, then compiles them with fake regulators, PMGR and MMIO under
UBSan and `-Wall -Wextra -Werror`. Its 18 cases cover successful activation,
repeat activation, optional absence, deferred/failed acquisition, enable
failures, both gate failures, preservation of existing references, failed
rollback, power-off failure and subsequent cleanup. No electrical behavior
or successful EHCI enumeration is established by those tests.

The extended test entry point has four unittest tests, all passing without
skips on this host. In addition to the 18 existing host power cases, it runs
the actual PMGR helper, register-dump helper and OTG initialization function
against fake register arrays: a 1,000-poll timeout produces no PHY reset
writes, and an acknowledged retry produces exactly the previous reset values
and delays while preserving both tuning registers. A temporary historical
control reverses only the checked-return correction and deliberately fails
the same contract with exit status 42: `return=0, PHY writes=2, post-gate
delays=4`. It never edits the production source.

Four further cases compile the complete production probe and distinguish an
absent second resource, mapping failures `-ENOMEM` and `-EBUSY`, and a mapped
resource. The fake mapping/provider callbacks do not map physical memory.
These tests use UBSan and strict compiler warnings; they establish software
error propagation, not actual hardware acknowledgement, voltages or routing.

## Reject matching states that are not ON

The gate helper requested state `0xf`, but accepted any equal request/status
nibbles. A readback of `0x00` (including an unresponsive register that reads
zero) or `0x44` could therefore report success and allow OTG PHY reset writes
although the requested ON state was not observed. This is a reproducible
software defect; neither readback has been observed as the cause of a failure
on our device.

The helper now requires both nibbles to equal the requested ON value `0xf`.
The existing gate request, preserved upper bits, 1,000-poll bound and error
propagation remain unchanged. No rollback, power-source policy or new register
write is added. EHCI/OHCI use the same helper and retain their existing error
cleanup. This does not qualify external USB host mode or Lightning VBUS.

The actual-C regression covers all 256 request/status pairs, acknowledgement
at the last allowed poll, and acknowledgement beyond the deadline. A temporary
copy restoring only the old predicate reproduces `status=0x00`, return zero
and two PHY reset writes; the new predicate times out before reset instead.
The full focused entrypoint passes six tests: 282 positive-contract cases and
two historical negative controls, compiled with UBSan and strict warnings.
The changed driver also builds and links as an ARM module against the prepared
Linux 6.12 tree, with no warnings, in an isolated two-CPU/1-GiB container.
The module was not loaded and no new kernel image was booted.
