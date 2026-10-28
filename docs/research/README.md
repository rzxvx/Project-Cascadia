# Research notes

Working notes from the bring-up, kept verbatim rather than tidied — the failed
attempts are the useful part.

Several of these originate from **teutekeune/iBSSloader** and were extended
during Cascadia work (notably `p105-smp-bringup.md`, which carries the lab
results of ~20 failed attempts at waking cpu1). Both projects are GPL-2.0.

## P105 power, storage and USB research

The following additional notes record offline work on iPad2,5 / p105ap. They separate driver
fixes, experimental build profiles and firmware evidence. Nothing here is
evidence of a successful physical charging, external USB-host or writable NAND
test. The normal boot procedure remains [QUICKSTART](../QUICKSTART.md).

The [deeper solution paths](p105-deeper-solutions.md) describe the newly
implemented, isolated Linux-C components for current control, PPN program/erase
and external USB-client/role handling. The normal build does not enable them.
[Actual offline UBIFS images](p105-linux-owned-rootfs.md) complete the file-build
layer without formatting or attaching a device.

| Area | Implemented and tested offline | Still unresolved |
| --- | --- | --- |
| Battery/PMU | Coherent high–low–high HDQ words, atomic cache, valid percentage range, break failure handling, discharge status, GPIO errors, removal of event-consuming probe dump; isolated current/IRQ controller | Real source/gauge producer, analog retention, complete charger initialization, battery/fault protection, voltage/termination policy and hardware verification |
| NAND | Read progress/ECC errors, PIO completion/tail-copy errors, failed-payload publication gates, explicit read-only MEMREAD backend with exact sysfs/device identity, strict metadata/cache checks, reduced mapper memory, NBD bounds/setup/worker cleanup, pidfd control and generation-bound System readiness | Actual process/sysfs hardware integration, target capacity/performance, writable PPN/FTL and Linux-owned storage |
| USB | Host-only build/link, bounded ADT parser, PHY power-error handling, two-bit GPIO selector masks, explicit input/IRQ peripheral-mode exit, isolated HID/storage profile | Hardware verification of the new reference-recovery path, complete connector routing, external 5 V VBUS/protection, role transitions and real peripherals |

Charging evidence:

- [Battery percentage validation](p105-battery-validation.md)
- [D1946 current and battery policy](p105-charging.md)
- [PMU Wake/Sleep/Quiesce](p105-pmu-lifecycle.md)
- [Source-event notifications](p105-source-events.md)
- [Input-current backoff, stored cap and deadline](p105-source-backoff.md)
- [Actual zero-input apply and communication-failure boundaries](p105-zero-input.md)
- [Actual source-budget construction and charger dispatch](p105-input-dispatch.md)
- [Actual voltage/clamp owners and permanently blocked charging contract](p105-charger-evidence-contract.md)
- [Charging status and blocked recovery paths](p105-charge-status.md)
- [Concrete charger guard-field owners](p105-charge-guard-ownership.md)
- [System watchdog and charger protection distinction](p105-pmu-watchdog.md)
- [ADC identities and scaling with acquisition blocked](p105-d1946-adc.md)
- [ADC-to-owner voltage software units and compensation](p105-voltage-unit-chain.md)
- [Boot PMU readback errors and bounded initialization coverage](p105-boot-pmu-init-boundaries.md)
- [ADC completion, IRQ and quiesce with fake I/O](p105-d1946-adc-lifecycle.md)
- [ARM clock interval conversion with fake frequency and time](p105-clock-intervals.md)
- [USB gadget budget integration gap](p105-usb-charge-budget.md)
- [Host current-offset request receiver](p105-usb-current-request.md)

Storage and USB evidence:

- [Read-only Linux storage and NBD limits](p105-linux-storage.md)
- [MLC media type and strict read-only compatibility](p105-mtd-media-type.md)
- [PPN error-list fields and unresolved AP/IOP ownership](p105-ppn-error-list-evidence.md)
- [PPN translation/cache/completion and panic boundaries](p105-ppn-completion-boundaries.md)
- [PIO read-end errors and cache publication](p105-ppn-read-end-errors.md)
- [PIO partial-tail copy and exact delivered bytes](p105-ppn-data-out.md)
- [NANDctl failed-read output and file-preservation gates](p105-nandctl-read-failures.md)
- [Strict synthetic error-list decoder and independent oracle](p105-ppn-synthetic-decoder.md)
- [Normalized multi-list observations and unknown absence](p105-ppn-normalized-observations.md)
- [Existing NAND architecture](p105-nand.md)
- [USB host/PHY evidence](p105-usb-host.md)
- [DWC2 versus Apple EHCI/OHCI controller boundaries](p105-usb-controller-boundaries.md)
- [Lightning accessory-side voltage conversion and missing wiring](lightning-accessory-conversion.md)
- [PHY error-path assumptions](p105-usb-phy-errors.md)
- [Historical partial-off/Core-count diagnosis and shared lifecycle](p105-usb-shared-lifecycle.md)
- [Implemented PHY reference recovery and real HCD regression](p105-usb-phy-reference-recovery.md)
- [Temporary compensation policy and actual USB consumer limits](p105-usb-poweroff-policy.md)
- [Actual Apple shared-PHY client arbitration and controller ownership](p105-usb-phy-ownership.md)
- [EHCI/OHCI controller and port power](p105-usb-ehci.md)
- [THS7383 accessory routing and orientation](p105-tristar-routing.md)
- [Accessory rail callbacks and ID-derived voltage](p105-accessory-power.md)
- [Accessory switch GPIO descriptor and simulated provider](p105-accessory-gpio.md)
- [SoC GPIO mux-mask comparison and remaining policy differences](p105-soc-gpio-masks.md)
- [GPIO input/IRQ lifecycle and peripheral-mode exit](p105-gpio-input-lifecycle.md)
- [DWC2 platform/HCD shared-PHY lifecycle and scope](p105-dwc2-phy-lifecycle.md)
- [VBUS presence sensing and the missing 5 V provider](p105-vbus-presence.md)

Run `python3 scripts/test-offline.py` from the repository root. It runs all
`tests/test*.py` entry points, including names with hyphens. C fixtures use a
native compiler and sanitizers; relevant integration fixtures also need the
pinned local Linux v6.12 source. Firmware experiments require Unicorn and a
separately obtained official p105 12H321 kernelcache. Each experiment validates
its SHA-256 before using fixed addresses. Missing prerequisites are explicit
skips, and skips do not establish correctness.

Instruction tests execute selected original code against fake RAM, PMU I/O or
provider callbacks. Their reports name the mocks; a software budget, routing
byte or PORTSC power bit does not prove an electrical voltage/current. The
firmware under study is iOS 8.4.1, and fixed addresses must not be transferred
to another iOS build. No firmware binaries, keys or real device dumps belong
in the source repository.

- [PPN durability: primary sources and E9 scope](p105-ppn-durability-primary.md) — commit/status comparison; no physical receipt or writable backend.

- [USB PHY clock source in the actual boot code](p105-usb-boot-frequency.md) — independent12H/13G producer evidence; no live clock binding.

* [Actual P105 PMGR44 diagnostic table ABI](p105-pmgr-table-abi.md) — corrected
  offline mapper and original-blob negative control; no clock activation.

* [Staging loader stack reservation](p105-staging-stack.md) — ARM prologue
  negative control and linker overlap rejection; offline only.

* [Owned USB-to-charger invalidation](../../experiments/p105-source/NOTIFICATIONS.md)
  — atomic epoch/work callbacks with RCU-drained ownership; physical timing unproven.

* [Native CDMA resource/IRQ/translation binding](p105-ppn-cdma-binding.md) —
  independently verified12H/13G evidence; physical ownership/durability separate.

* [CDMA completion and retained DMA lease lifetime](../../experiments/p105-ppn/COMPLETION.md)
  — actual engine adapter tests; separate external quiescence receipts required.

* [Bounded owned-USB2 current candidate](../../experiments/p105-current/USB2_CURRENT.md)
  — further default-false100/500mA plan; stock gadget budget/protection remain open.

* [Native PMGR clock-column addresses](p105-pmgr-clock-column.md) — packed-index
  guards and real+0x38 base; correctedUSBREG mapping, no device action.

* [Checked native host clock broker](../../experiments/p105-native-clock/DESIGN.md)
  — actual parent graph, shared client accounting and retained fault state;
  default denied, global-owner adoption still required.

* [True clkG factory and native parent bridge](p105-true-clkg.md) — actual
  original dispatch and CPU-idle negative control; registry association separate.

* [IOP halt, CDMA and boot handoff](p105-iop-handoff.md) — separate namespaces,
  unchecked native stop callbacks and early sleep ACK; no invented DMA fence.

* [Optional CDC2 USB2 declared demand](../../experiments/p105-gadget/EVIDENCE.md)
  — default0, checked opt-in100/500mA declarations and actual EP0/source tests.

* [Concrete feature integration order](p105-feature-integration.md) — implemented
  source components, exact join points and remaining physical qualification.

* [Native clkG registry-owner pipeline](p105-clkg-registry.md) — actual startup
  publisher/lookup/factory dispatch with absent/wrong-marker controls; live
  Linux/global-owner binding remains separate.

* [Checked wall-source observation](../../experiments/p105-wall-source/EVIDENCE.md) — actual Tristar routing/ADC classifier; retained restore ownership, no current grant.

* [Checked ADC6 polling](../../experiments/p105-adc6/EVIDENCE.md) — actual PMU43/44 conversion, bounded checked cleanup and retained uncertainty; no real all-ADC owner yet.

* [Checked CDMA cancellation](../../experiments/p105-ppn/CANCELLATION.md) —9,118 actual-C assertions/288 original traces; successful transport still WAIT_FENCE.

* [Native IRQ retirement boundary](p105-cdma-irq-retirement.md) — mask-set is not a captured-callback drain.

* [Boot CDMA startup](p105-boot-cdma.md) — actual BSS clear and clock-gate path, distinct from inherited hardware reset/fence.

* [Lightning digital-ID record](../../experiments/p105-accessory-id/DESIGN.md) —4,360 original semantic cases; debugger/ignore-vbus/ramdisk context, no adapter identity or role/current grant.

* [Earlier boot reset/AIC initialization](p105-boot-early-reset.md) — native accepted-ID matrix and actual initializer calls, no inferred DMA fence.

* [Linux IRQ synchronization scope](p105-linux-irq-sync.md) —189/192 assertions against actual GPL C; AP handler/thread drain is separate from hardware and native IOP state.

* [Actual END/TO recovery](p105-eoc-recovery.md) — original dictionary/mode/pulse and full caller error behavior; Linux retains faults.

* [Global D1946 bank owner](p105-dialog-banks.md) and [checked Linux bank0 transport](../../experiments/p105-bank0/EVIDENCE.md) — owner-bound cached selection, retained failure, no analog activation.

* [Remote accessory signature exchange](../../experiments/p105-authcp-aid/AUTHENTICATION.md) and [original verifier control flow](../../experiments/p105-authcp-aid/CRYPTO_BOUNDARY.md) — unverified bytes, fake crypto, no role/current grant.

* [Actual selected kDFU sleep request](p105-kdfu-sleep-boundary.md) and [FMI client retirement](p105-fmi-client-retirement.md) — original software ordering without physical DMA/IRQ fence.

* [Retained DMA diagnostics](p105-retained-dma-diagnostics.md) — blocked program/erase calls preserve buffer-retention warnings without attempting new hardware work.

* [Checked THS IDbus primitive](../../experiments/p105-ths-idbus/DESIGN.md) — Linux regmap submit/receive, checked bulk errors, request identities and software revocation; real IRQ/lease binding remains unprovided.
