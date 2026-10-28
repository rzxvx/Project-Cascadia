# P105 PPN 1.5 row-address cycle mismatch

**Source-qualified candidate; physical qualification is pending.** The kernel
and userspace PIO page readers both sent three row-address bytes. The original
firmware selects four for the PPN 1.5.x profile documented for this port, even
when the fourth address byte is zero. This patch changes that count in both
`0A ROW 37` readers. It does not change the row value, geometry, command
allowlist, status interpretation, data transfer, or controller restoration.

The port still assumes a fixed NAND profile. The connected iPad's actual
protocol revision and geometry have not been identified by these tests. This
candidate must not be treated as a generic version detector or a qualified
replacement for every iPad NAND variant. Older PPN revisions have a different
selection rule; retain the draft until the applicable profile is verified.

## Original firmware evidence

All addresses are from the specifically identified **iBEC** images, not from
an iBoot image or a neighboring firmware revision:

| Image | Complete plaintext SHA-256 |
| --- | --- |
| iPad2,5 12H321 iBEC | `30c040052c444222edc8646316f95a355a9e338d6aadc7168f8a7acc7c4578ec` |
| iPad2,5 13G36 iBEC | `17020be7b0fa2d67637e00106254002084f9b51925ba1d91e10a95bf5101a0a8` |

In 12H321, `0x9ff03116..312a` packs ID bytes 3,4,5 as
`major << 16 | minor << 8 | patch`. The value is passed as the final stack
argument at `0x9ff0343a..343e`; channel setup stores it in the PPN object at
offset `0x200` at `0x9ff089a8`. The geometry reader then selects the widths:

| Selection | 12H321 | 13G36 |
| --- | --- | --- |
| Version load / compare | `9ff084ba..84c6` | `9ff08a16..8a22` |
| Compared literal | `0x000104ff` at `9ff08800` | `0x000104ff` at `9ff08d54` |
| Older-version geometry sum / ceil to bytes | `9ff084c8..84da` | `9ff08a24..8a36` |
| Newer-version row count = 4 | `9ff08502..8504` | `9ff08a5e..8a60` |
| Row-plus-column count = 8 | `9ff08508..850e` | `9ff08a64..8a6a` |

The selected row count is stored at PPN-object offset `0x260`; the combined
count is at `0x261`. For the documented ID `PPN 01 05 05`, the packed revision
is `0x010505`, above the cutoff. Five original arithmetic/selection slices per
image were executed offline for versions `010400`, `0104ff`, `010500`,
`010505`, and `010600`: all ten cases selected the expected 3/7 or 4/8 widths.
Only synthetic RAM and the original instruction bytes were used; no helper
mock or MMIO was involved. This proves the selection, not the unit's ID.

The width is consumed by an actual read command, not just stored unused:

- The direct 12H321 bootpage reader loads `0A` and `37` at `9ff0790c..790e`, loads
  `PPN+260` at `9ff07910`, and passes it to the command/address helper at
  `9ff07914..7918`.
- The normal sequential read function loads `PPN+260`, subtracts one and
  queues it at FMI offset `c0014` at `9ff0a038..a04a`. The same function
  queues `370a` at `9ff0a29a..a2da` (or `3707` for preceding pages).
- Both Linux command/address helpers encode an N-byte address as
  `(N + 7) & 7` in `FMC_ADDRNUM`. Passing three therefore writes 2; passing
  four writes 3. Writing a 32-bit `FMC_ADDR0` alone does not select four cycles.

Earlier bring-up notes and the PIO implementation used three bytes. Those
historical read observations are not discarded, but they do not explain why
that abbreviated command was tolerated. The native fixed-profile selection
is the basis of this candidate. Firmware bytes remain outside the repository.

## Offline regression and build scope

`python3 tests/test-ppn-row-cycles.py -v` compiles the actual two page-reader
functions and their actual command/address helpers under UBSan and Werror.
The local register/API fixtures check four row values, successful reads,
command failures and status failures: 24 transactions in two groups. Both
groups pass; unchanged main fails both at the encoded-address-count check.
The row, CE release, end command, data-transfer gating and saved-state
restoration are checked alongside the count. The fixture does not establish
physical bus timing or data validity.

A complete ARM kernel module build with `W=1`/`-Werror`, and an ARM userspace
`ppn.c` object build with `-Wall -Wextra -Werror`, passed without warnings.
Builds used at most two CPUs and 1 GiB, with original kernel sources read-only.
No driver, firmware or tool was transferred to the iPad.

## Relation to the current read failure

Four earlier bounded reads on the running frozen kernel returned status
`0x50`, ECC-failure counters and invalid data. Existing boot logs show
`FMC_ON=5` on both buses; that does not qualify the rest of the inherited
interface state. The address-width discrepancy is a concrete investigation
lead, **not a demonstrated cause or repair of those failures**. No further
NAND reads or controller resets were made for this work. NAND program/erase,
partitioning and Linux filesystem installation remain excluded.

The late transport-error fixes in #9 and unknown-status handling in #13
remain independent. This count-only patch must retain those corrections when
combined; it does not replace them.

## Further qualification of the observed status

The same two hash-pinned iBEC images independently name the direct function
`h2fmi_ppn_read_bootpage`. In this operation, a successfully obtained status
of `0x50` takes the bit-4 error branch, whose literal is
"General Error bit asserted during Read Boot Page". It loads internal result
`0x80000020`, rather than accepting the page or retrying the read command.

| Step | 12H321 iBEC | 13G36 iBEC |
| --- | --- | --- |
| Function entry | `9ff078d0` | `9ff07ce4` |
| Exact-success / bit-4 tests | `9ff0792c` / `9ff07930` | `9ff07d52` / `9ff07d56` |
| Error log / result load | `9ff0793e` / `9ff07942` | `9ff07d94` / `9ff07d98` |
| Error string | `9ff3658e` | `9ff39b51` |

The subsequent three-step data copy advances its destination, without issuing
another page-read or returning to the status decision. It preserves the saved
error if data transfer succeeds; a later transfer failure can replace it with
`0x80000022`. The extra ECC check is reached only for the success result, so
this source does not identify the general error as a physical ECC failure.

An additional offline check executes only the original branch-selection
instructions: all 256 status bytes with synthetic status-reader return 0/1 in
each image (1024 cases), plus four nonzero-return controls. Execution stops
before logs, helpers or the common result path. Only image RAM and a synthetic
stack are mapped; there are no helper replacements, NAND or MMIO mappings.
The final error constants and preservation path above are separately checked
statically, not claimed as a complete emulated read.

This is operation-specific evidence. For example, `0x42` enters this bootpage
path's other-error branch, although the Linux normal reader labels it REFRESH.
Neither the branch replay nor the repeated numeric value `0x50` in a controller
control-word write justifies changing Linux's accepted statuses. The connected
unit's physical fault cause, protocol profile and suitable recovery remain
unidentified. No new read or reset was attempted, and this documentation update
changes neither the candidate code nor any recorded ARM artifact.

## Fixed bootloader cache: source qualification and blocked observation

A narrower way to seek the unit's revision was investigated without another
NAND transaction. In the hash-pinned 12H321 iBEC above, `h2fmiInit` copies
eight bytes of the initial bus-0 READID buffer to static RAM `0x9ff50000`
at `9ff03042..304a`. The copy precedes the PPN-prefix check and subsequent
bus-1 fallback/merging. `h2fmiPrintConfig` at `9ff03ab4` reads the first six
bytes there; bytes 3/4/5 encode the cached revision. This is historical bus-0
information, not a witness of successful initialization or current geometry.

The actual uploaded plaintext Img3 has SHA-256
`22237959161dab1eb514962bd4d5e498830470050af026cc4f95c83a6e3ee9c7`.
Its DATA payload starts at offset 64; 78 selected code/literal regions match
the original image, including this copy and the entry BSS-clear sequence.
Regular ARM entry clears `[9ff44840,9ff535e8)`, which includes the cache.
The frozen loader contains data-cache clean/invalidate and L2 clean handling.
The cache is above declared Linux RAM; these source checks do not prove its
complete later lifetime or physical preservation. No dynamic FMI/PPN pointer
or heap region was followed.

On 2026-10-14, a private one-shot helper was reviewed and checked with eight
actual-C stubbed-I/O/argument controls, then built as strict static ARM code.
It opens `/dev/mem` read-only and makes exactly one six-byte `pread` at this
fixed address, with no retry, scan, mmap, pointer follow or write. Only this
helper was staged in a temporary `/run` tmpfs directory; it is not firmware.
With battery 58% and temperature 31.3 C, that single call returned `EFAULT`
("Bad address"). No ID bytes were obtained. Its own file and directory were
removed immediately, and no alternative mapping or repeated read was tried.

Linux 6.12 `drivers/char/mem.c:read_mem` rejects an invalid physical range
with `EFAULT` before copying. ARM's `arch/arm/mm/mmap.c:valid_phys_addr_range`
limits reads to `__pa(high_memory - 1) + 1`; `arch/arm/mm/mmu.c` derives
`high_memory` from `arm_lowmem_limit`. The live `/proc/iomem` lists System RAM
only through `0x9f5fffff`. The refusal is consistent with the bootloader cache
being outside Linux's low-memory range; the exact returning branch was not
instrumented. There was no attempt to relax the kernel boundary.

This negative observation does not identify the PPN revision or validate the
four-cycle candidate. Draft status and all device-profile qualification
requirements remain. No NAND command, controller reset, program/erase or
new boot occurred, and the candidate's production code and ARM artifacts
are unchanged.
