# PPN durability: focused primary-source follow-up

Date: 2026-10-12. Source-only research. No device, NAND, MMIO, boot, driver,
firmware upload, bus reset or programmer action occurred. Existing frozen C groups
are unchanged. The useful new result is a **verified E9 scope correction** and an
independent Apple architectural description of commit/status ordering. None of
these sources authorizes a hardware durability receipt.

## Firmware identity is not our unit's identity

Upstream `docs/research/p105-nand.md` reports the author's hardware as read-ID
50504E010505, manufacturer AD8252232102, firmware `040472P_HYNIX_B_`, marketing
`20nm64Gb`. Those observations motivate Hynix research; they do not identify our
connected unit. Our unit's NAND chip, capacity, geometry and package firmware
have not been read. The generic PPN signature must not be fed to a raw-NAND
manufacturer/geometry decoder. No inference from A1432 or iPad2,5 fills that gap.

## New exact source result: E9 is debug/GEB setup in 12H321

`verify-e9-scope.py` checks 22 original instructions, literals and diagnostic
labels against both complete artifact SHA256 gates:

- Official 12H321 kernelcache: `db1ddc8343aa9debc00e0a083fba59d9f555722db60957dc333b854fac21dd2c`.
- Embedded IOP firmware: `6dfa5dcc6f8cb38be41e48c0edc543b2014a026f7909c35c9ef050ffd23c22d4`.

IOP offsets use the existing relocated-zero convention. At 6a8a helper 6a70
sends E9 through the three-address-byte helper66bc. It transfers data through
PIO367c, waits6b28, then sends E7 and7D and calls status reader692c. Its direct
caller7b72 passes address F0FFFF and a four-byte page payload. The caller's
original diagnostic binds it to `h2fmi_ppn_force_geb_address`; the callee's
REQUIRE label binds it to `h2fmi_ppn_set_debug_data`. The caller subsequently
sets controller context state at208/20c. This is a concrete SET_DEBUG_DATA / GEB
force path, **not evidence of normal page programming or a persistence flush**.
A linear Thumb scan located one direct BL caller; indirect-call absence is not
proved. No new interpretation of GEB internals or replay safety is asserted.

The helper returns1 after the status-reader call without testing its result
(6ae8 followed by6afa). This is another reason not to borrow its success result
as physical completion. E9 remains outside the experiment's operation scope;
there is no change to production allowlists or callbacks.

## Primary source evidence matrix

Each publication below is an architectural or algorithmic example, not a data
sheet for the unmeasured target. Numbered tables/figures let another reviewer
locate the specific positive evidence. No command meaning is transplanted solely
because the same number appears in a patent.

| Primary source | Positive evidence | Exact limit for this backend |
|---|---|---|
| [Apple US8862851B2](https://patents.google.com/patent/US8862851B2/en), priority2008, Tables3–5 and Figures4/7 | Separates buffered/noncommitted writes from commit to an addressed memory array and subsequent address-bound completion. Status Table4 distinguishes ready6, operation error0, refresh1, retirement2 and address error3. Table5 includes87/17 page write with address. | Actual original 12H321 non-final page commands87/17 match; its final8A variant, EF3380 and device power-loss guarantees remain unspecified. An example architecture is not a particular package firmware contract. |
| [Apple parent US8370603B2](https://patents.google.com/patent/US8370603B2/en) | Confirms the earlier publication in the same address-mapping family. | Same family; it is not independent validation or a second hardware test. |
| [SanDisk US9003102B2](https://patents.google.com/patent/US9003102B2/en), Figure11 and event-log section | Distinguishes command status from accumulated foreground/background events. FAIL may announce event-buffer pressure even after a successful command. Examples include last-good-page, write-abort and timeout details. | Its illustrative8KiB TLV event log is not our three512-byte count/page lists. No3380 definition or clear/flush proof follows; absent list entries cannot prove success. |
| [SanDisk CN102473126A](https://patents.google.com/patent/CN102473126A/en), Figure13D and write-abort discussion | Controller-ready is reported after NAND program results; package/host bad-block responsibilities can differ. Interrupted MLC upper-page programming may damage older lower-page data; safe-zone copying or power hold-up are separate mechanisms. | No Hynix/Samsung/Toshiba package-specific status register, SLC alias rule, error-list ABI or EF3380 handshake is specified. |
| [Samsung US10147495B2](https://patents.google.com/patent/US10147495B2/en), Figures16–19 | Distinguishes ordinary program status from additional loop-status soft-failure detection and mentions a PPN implementation. | Cannot map its internal PS/LS bits or recovery command to A5 raw40/41/42/45 or3380. Program-pass is not automatically every reliability check. |
| [Samsung US10509575B2](https://patents.google.com/patent/US10509575B2/en), Figure20 | Describes metadata flush triggered by power-off/sleep or other conditions in a system that can include PPN storage. | Host-memory-buffer metadata persistence is a different layer; its use of “flush” supplies no low-level PPN opcode. |
| [Samsung US20130010539A1](https://patents.google.com/patent/US20130010539A1/en), Figure23 | Gives a PPN package/controller architecture with DDR host channels and internal ECC. | No E9/3380, crash-safe SLC programming, current target chip identity or parameter ABI. Endurance examples are not target specifications. |
| [Apple US8489907B2](https://patents.google.com/patent/US8489907B2/en), Figure2 | Package controller and arrays may have separate power domains and power-cycle requests. | A generic controller-requested cycle is not permission for an A5 bus reset, a NAND cache flush or evidence of DMA quiescence. |
| [Apple US8411519B2](https://patents.google.com/patent/US8411519B2/en) | Selective block retirement is a distinct host/storage policy. | No physical bad-block receipt can be synthesized from a generic transfer failure. |
| [Skorobogatov's original Cambridge paper](https://arxiv.org/pdf/1609.04327), IV.A/D | Real Hynix iPhone5c PPN read/erase/write implementation and same-block SLC/MLC-like alias observations. | Different A6 chip; no released programmer source,3380 semantics, SLC durability or measured A5 alias exclusivity. Nothing concerning passcode attack implementation is adopted. |
| [Medusa's own2020 release](https://medusabox.com/eng/news/details/356) | Lists mini1 presets and PPN read/erase/write support for Samsung KLE/K9HF, Toshiba THGBX and Hynix H2J parts. | Closed-tool capability announcement, not source/ABI or power-cut proof. No download, execution, purchase or use is proposed. |

## Original code correspondence, kept distinct from patent claims

Our already frozen actual-original 74-run program emulator checks87 on non-final
pages and8A on each queue group's last page, with17 as the emitted second command
(`research/ppn-protocol/emulate-iop-write.py`119–123). The single-page SLC plan
therefore uses final8A/17. The patent's87/17 match is useful naming/order
corroboration; it does not prove that8A is identical to the patent's unspecified
last-stride completion, or supply undocumented power-loss rules.

Actual EF3380 evidence remains the separate pinned setter5f50 and conditional
write-error callers6550/93b8 described in `PPN_ACK_BOUNDARIES.md`: transported
four-byte word1, called after error/list handling, setter result not used to
return successful program completion. It is not an ordinary successful-path
flush. None of the newly examined sources identifies3380 as a durable ACK,
proves idempotence or shows that reissuing it cannot discard unreconciled errors.
The current pure C engine remains `durability_unknown`; no hidden reset, recovery
write or ACK callback has been added.

## Actual other open projects

[nabla-c0d3/iphone-dataprotection](https://github.com/nabla-c0d3/iphone-dataprotection/tree/572dd5cd8c07f5f14f7ea9488041031dd22a26bb)
was examined at `572dd5cd8c07f5f14f7ea9488041031dd22a26bb` in a separate clone.
`ramdisk_tools/ioflash/ioflash_kernel.c` explicitly accepts only read-page calls,
then delegates to Apple's kernel. Its headers name write/erase enums but do not
implement PPN bus programming. `python_scripts/nand/ppn.py` builds an offline
logical map and reads it. It supplies no Linux PPN writer or durability barrier.
No patcher or downloaded script was run.

[OpeniBoot](https://github.com/iDroid-Project/openiBoot/blob/866562fdb1cfd019bcd77885c80fbf0af65d5c15/plat-a4/h2fmi.c#L3029)
recognizes the PPN signature and returns before supporting it, lines3029–3032.
Its raw-NAND writer/CDMA code cannot be taken as a PPN contract. Its GPLv3 code
has not been copied into the GPL2 experiment. Sandcastle's later Hx NVMe path
remains a different interface, not an A5 PPN implementation.

## Narrow negative coverage

Searches included exact EF3380/0x3380, PPN+E9/E9h, PPN+error-list, exact upstream
firmware/ID strings, Samsung KLEAG2GW1A, Toshiba THGBX2G7B2JLA01, Hynix H2JTCG,
manufacturer domains and “perfect page” patent families. No package-specific
vendor data sheet or open implementation closing3380/flush/power-cut semantics
was found in the material actually inspected. This is scoped coverage, not a
claim that no such document or private implementation exists. SK Hynix search
hits often use PPN merely as “physical page number”; they were not promoted to
Apple PPN evidence. Forums/programmer logs were leads, not accepted ABI proofs.
Official ONFI document/specification endpoints rejected access; indexed opcode
reservation snippets and raw-NAND cache-program documents cannot define this
16-bit-feature PPN interface. No unknown firmware was downloaded.

## What changes in the minimal backend contract

The stronger source-based requirement is: bind completion to the **exact issued
physical page/CE and commit stage**, preserve accumulated error observations,
and exclude debug/GEB operations from the success path. A mere ready bit, host
FIFO/CDMA drain, successful feature transport, valid page CRC, readback while
powered, or a higher journal generation cannot independently create a
`durable` receipt. This is an engineering conclusion from the separated evidence,
not a manufacturer guarantee.

To close the hardware gate, a future backend still needs the exact target
package/firmware contract for (1) final page commit and array-complete status,
(2) list validity/overflow/ownership and3380's precise state effect, (3) SLC-only
mode/address/physical-alias exclusivity and allowed page order, (4) interrupted
program/erase effects on neighboring/older pages, and (5) recovery/retirement
rules. Manufacturer documentation or separately authorized sacrificial hardware
qualification must provide that source of truth. No such qualification is part
of this work, and no test on the user's iPad is started or requested here.

The endian-safe codec, BBTv3, bounded command engine, CDMA planner and sequencer
planner close software prerequisites. They deliberately do not solve these
package-level gates by inventing success. Linux-own rootfs still requires real
SLC qualification (or a suitable MLC-aware architecture), truthful MTD geometry,
UBI placement and an independent boot/recovery route if iOS is removed.

The public protocol engine remains unbound with unknown durability. E9 is not
added to it. Original instruction verifier/facts remain private research inputs;
only this source-attributed report is exported.
