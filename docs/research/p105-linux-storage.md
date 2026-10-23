# P105 Linux storage: read support, write blockers and offline fixes

Assessment: 2026-10-11. Cascadia base revision:
`a9bf28f20e5ce908fd5805e7d58b3ce9d9f0e4cf`.
Local branch: `research/p105-power-storage-usb`.

The current device is iPad2,5 / P105AP on iOS 9.3.5 (13G36), without a Pico.
All work in this note is source analysis and synthetic host tests. No NAND
dump, device boot, filesystem mount, device ioctl or hardware write was done.
The user's original restrictions still forbid NAND program/erase, partition
changes and replacing iOS. An overnight research request is not permission to
perform those operations.

## What exists, and what does not

| Layer | Current evidence | Remaining work |
|---|---|---|
| H2FMI + Hynix PPN | Kernel read-only MTD driver; PIO page reads, OOB metadata and page status | DMA/IRQ optimization, hardware validation on this device |
| PPN ECC/status | Package corrects data internally; 0x42 requests refresh, 0x45 retired, 0x49 erased | Write/erase failure semantics, refresh and retirement handling for any future writable design |
| iOS disk reconstruction | Metadata-based LBA map, offline Python scanner and read-only `iosnand` NBD server | Full writable AppleSwissPPNFTL, context updates, GC, wear leveling, discard semantics and power-fail recovery are absent |
| LwVM | Read-only header/chunk map reconstruction of System/Data | No supported coexistence repartitioning method |
| Filesystem | Unencrypted upstream System HFSX is reconstructable | iOS Data uses per-file content protection; a recovered image is not proof that protected files are usable |
| Linux-owned NAND | Upstream proposes isolated raw flash + UBI/UBIFS after giving up iOS | No writable PPN driver, tested flash layout, proven MLC/SLC mode, safe boot/recovery path or hardware verification |

The geometry documented by upstream's **16 GB test unit** is two buses, two CAUs per bus, 1064 blocks
per CAU, 256 pages per MLC block, 16 KiB data plus 64 bytes metadata per page.
The nominal 16 GB logical iOS disk is an FTL product; `/dev/mtd*` is not that
disk. HFS+/ext4 cannot simply be mounted on the raw PPN page stream. A1432 /
iPad2,5 alone does not identify capacity; our unit's capacity has not been read.

The current kernel driver and both reconstruction tools remain specific to
that 16 GB geometry. LwVM's packed `mediaSize` is at header offset 0x20;
[openiBoot's structure and chunk calculation](https://github.com/iDroid-Project/openiBoot/blob/866562fdb1cfd019bcd77885c80fbf0af65d5c15/includes/bdev.h)
and `bdev.c` independently identify it. Allocation chunk size depends on media
size (16 MiB for the documented 16 GB medium); merely increasing `NLBA` would
not make the current decoder support a different capacity. The local parsers
now reject a declared media size other than 16,000,000,000 bytes explicitly.
Before real NAND use, confirm this unit's capacity and read the PPN device
parameters/controller info through an authorized read path. Dynamic geometry
support is not implemented or claimed.

### Read-only means no NAND programming, not zero hardware state changes

The kernel driver has only `_read_oob` and `_block_isbad`, `MTD_NO_ERASE`, no
`MTD_WRITEABLE`, and a command allowlist excluding program/erase. It does alter
controller registers for PIO, polling and acknowledgement. Userspace `ppn.c`
additionally has a tightly restricted EF feature 0x180 interface-power-state
path; `nandctl ddr` can use it. Reset and mode changes can lose the iBEC setup.
None of these tools were run against a device in this session.

`iosnand` sets `NBD_FLAG_READ_ONLY` and refuses write requests; it does not
implement an FTL write path. Its scan cache is a host/Linux filesystem file,
not an iOS NAND modification. NBD `FLUSH` returns success because there are no
accepted NAND writes to flush. Attaching even a read-only block mapping still
requires a running device and privileged ioctls, outside this research run.

Kernel MTD's per-bus mutex and legacy `/dev/mem` tools' userspace flock are
different locks. Mixing a kernel MTD read with a direct PIO sequence can still
interleave commands. The legacy PIO backend requires exclusive use; a process
lock alone does not protect it from arbitrary `/dev/mtd*` users.

## Why an apparently unused area is not safe Linux storage

AppleSwissPPNFTL has global sequence numbers, allocation, context and garbage
collection. An unused LwVM allocation chunk is unused logical address space;
it is not a reserved physical eraseblock outside iOS's FTL. Writing raw pages
there while bypassing Apple metadata can invalidate the whole volume.

Shrinking the lower `fsys` flash partition would isolate physical blocks, but
the original FTL geometry/context then ceases to match its formatted layout.
Upstream describes restore/repartition complications and edits to the block
containing boot-critical flash metadata. This is a destructive architectural
alternative, not an extension of the read-only tool.

Without a Pico, the intended boot entry is a jailbreak/kloader on working iOS.
Destroying iOS also removes that entry. A separately demonstrated BootROM
entry/recovery path is therefore a prerequisite for any future iOS-sacrificing
installation, even before considering filesystem correctness. Current scope
keeps iOS and the kDFU dependency intact.

## Other primary projects: useful evidence, no drop-in writable A5 stack

### openiBoot / iDroid

Repository: [iDroid-Project/openiBoot](https://github.com/iDroid-Project/openiBoot),
reviewed revision `866562fdb1cfd019bcd77885c80fbf0af65d5c15`.

Its A4 H2FMI driver and YAFTL are useful references for controller timing,
buffer management and older raw-NAND storage layers. The A4 driver explicitly
returns at PPN detection with “We don't know no PPN”, and contains a PPN TODO.
That is decisive evidence against transplanting its write path to this P105
PPN. YAFTL is not proof of compatibility with AppleSwissPPNFTL. The project's
README also warns against running its A4 NAND write functions.

- [PPN detection and unfinished geometry path](https://github.com/iDroid-Project/openiBoot/blob/866562fdb1cfd019bcd77885c80fbf0af65d5c15/plat-a4/h2fmi.c)
- [YAFTL implementation](https://github.com/iDroid-Project/openiBoot/blob/866562fdb1cfd019bcd77885c80fbf0af65d5c15/ftl-yaftl/yaftl.c)

No code was copied from its GPLv3 implementation into Cascadia's GPLv2 source.

### Sandcastle

Repository: [corellium/linux-sandcastle](https://github.com/corellium/linux-sandcastle),
reviewed branch `sandcastle-5.4`, revision
`0c2f7dda13d67bb7e06123df516f8bdb1000a79b`.

The inspected storage path is Hx mobile NVMe via PCI, controller scratch
memory, flat DMA and NVMMU mappings. P105 H2FMI/PPN uses another generation of
storage protocol and controller. Sandcastle's NVMe code is not a PPN or
AppleSwissPPNFTL implementation and cannot solve A5 NAND writes by a direct
port. Only the inspected source files and revision metadata were downloaded.

- [Hx NVMe source](https://github.com/corellium/linux-sandcastle/blob/0c2f7dda13d67bb7e06123df516f8bdb1000a79b/drivers/nvme/host/hx.c)
- [Project documentation](https://github.com/corellium/projectsandcastle/blob/master/README.md)

### Linux UBI/UBIFS and the extra MLC blocker

[UBIFS documentation](https://www.kernel.org/doc/html/v6.15/filesystems/ubifs.html)
describes UBI wear leveling and bad-block handling on MTD, not on Apple's FTL.
In the exact [Linux v6.12 UBI attach source](https://github.com/torvalds/linux/blob/v6.12/drivers/mtd/ubi/build.c),
MLC NAND is rejected unless `MTD_SLC_ON_MLC_EMULATION` is set. UBI assumes a
storage programming model different from arbitrary raw MLC pages.

P105 advertises 256-page MLC and 128-page SLC blocks. That alone does not prove
how to select SLC, what capacity is available, whether lower/upper pages are
coupled, or whether interrupted programs damage previously acknowledged
pages. The read-only driver now reports `MTD_MLCNANDFLASH` for its fixed
256-page MLC mapping; it sets no SLC-emulation flag. The strict MTD backend
rejects older kernels that mislabelled this mapping as SLC, as well as
SLC-emulated or writable exports. Rebuild the matching kernel and tool
together; there is no compatibility override or silent PIO fallback. See
[MTD media-type evidence and compatibility](p105-mtd-media-type.md).
Internal ECC does not
establish power-cut atomicity. A future writable design must prove the PPN
mode/order/failure guarantees before UBI, without merely bypassing this gate.

### Write completion and power-loss integrity: unresolved protocol obligations

The locally extracted official 8.4.1 iBEC contains diagnostic names for
`h2fmi_ppn_write_bootpage`, `h2fmi_ppn_erase_blocks`, `fmiss_ppn_write_multi` and
`h2fmi_ppn_process_error_list`. Its messages distinguish **failed-program**,
**pending-after-program-failure** and **should-be-retired** page lists. This
establishes useful reverse-engineering targets, not a complete write protocol
or permission to execute those routines. Their queue/address/error-list
semantics are not yet reconstructed.

A future driver cannot treat “ready” as durable success without establishing
which queued operations completed, which pages remain pending, and how a
retired block affects older valid pages. It also needs documented retry rules:
blindly repeating a partially completed NAND program is not a recovery plan.
Boot pages use separate BCH/SDR handling and must remain excluded from a
Linux-owned data region.

Two distinct designs then need different proofs:

- **An Apple-compatible FTL writer** would need crash-consistent allocation,
  L2V/context checkpoint updates, replay, generation/sequence handling,
  obsolete-page retirement and GC that never erases the last committed copy.
  Replaying “highest observed sequence wins” is a read-recovery technique;
  it does not establish Apple's commit/discard rules.
- **Isolated PPN + UBI** would need stable eraseblock geometry and a proven
  SLC or equivalent safe managed-MLC programming model. UBI already provides
  sequence-number/CRC-based relocation and atomic LEB change, but cannot fix a
  lower layer that silently loses previously acknowledged data when a later
  paired-page program is interrupted. The physical interface must meet UBI's
  assumptions first.

The corresponding Linux reference is
[v6.12 EBA atomic change and relocation](https://github.com/torvalds/linux/blob/v6.12/drivers/mtd/ubi/eba.c).
Before hardware writes, an offline simulator should inject failures at every
program/commit/erase boundary and verify that one complete committed version
survives, metadata never references freed blocks, and recovery does not depend
on an intact iOS. Such a model needs actual PPN command semantics first; an
invented simulator of guessed semantics would not validate this hardware.

## Concrete local corrections

### Offline dump reconstruction (`tools/nand-ftl-scan.py`)

- A valid refresh-needed page (status 0x42) used to be silently skipped. The
  scanner now accepts 0x40 and 0x42 like the production PPN read layer.
- Truncated pages, missing/extra status bytes, geometry overruns, absent dumps
  and more files than the four-bit map index supports now fail clearly.
- Every mapped read validates page status, exact chunk length and FTL LBA plus
  sequence. A changed dump/file ordering now fails instead of emitting a
  mismatched logical block.
- LwVM signature/count, duplicate or missing logical chunks, physical bounds
  and partition sizes are validated. Partition extraction emits exactly
  `end - begin` bytes, rather than padding to an entire 16 MiB chunk.
- Negative/out-of-disk ranges fail before output creation. Maps and images
  are built in a private 0600 temporary file, flushed, and published with an
  atomic hard link that refuses an existing/racing output. A late failed read
  removes the incomplete temp and never publishes a misleading final image.
  No input or earlier recovery image is overwritten.
- A recovered zero for an unmapped LBA remains a recovery convention; trimmed
  LBAs can still expose old data. This does not claim a complete writable FTL.

### Read-only NBD mapping (`tools/nand/iosnand.c`)

The LwVM decoder is now separate from hardware reads and rejects malformed
partition counts, repeated/out-of-range chunk indices, holes and impossible
sizes. The previous zero-initialized missing chunk could select physical
chunk 0, i.e. the LwVM header, as partition contents. The previous chunk count
could also exceed the fixed `phys[1024]` array. Both paths now fail closed.
Partition/NBD request bounds use subtraction to avoid unsigned addition
overflow. Cached block counts and 48-bit block-head sequences are validated before
they are used as page-array bounds: a corrupt `written > 256` field previously
allowed a walk past the cached block. CLI syntax and read ranges are now checked
before `/dev/mem` is opened, so help, typos and invalid reads cannot initiate
hardware scans. No NAND opcode, write/erase callback or MTD flag was enabled.

### Exact chunk sequence ranking and cache v2

The PPN metadata has **four separate u48 sequence fields per page**, one in
bytes 2–7 of each 16-byte chunk metadata record. The previous C reader cached
only chunk 0's value and ranked other chunks as `base + k`, merely warning when
that guess disagreed with the actual bytes. A synthetic pair demonstrates the
failure: page A has base 100 and chunk-3 sequence 900; page B has base 500 and
chunk-3 sequence 700. The guess selected B (503 > 103), whereas the actual
metadata selects A (900 > 700). The Python dump reader already uses exact values.
Source evidence: [upstream measured metadata layout](https://github.com/rzxvx/Project-Cascadia/blob/a9bf28f20e5ce908fd5805e7d58b3ce9d9f0e4cf/docs/research/p105-nand.md#page-layout-and-the-ftls-metadata)
and [upstream per-chunk dump scan](https://github.com/rzxvx/Project-Cascadia/blob/a9bf28f20e5ce908fd5805e7d58b3ce9d9f0e4cf/tools/nand-ftl-scan.py).

`iosnand` now caches the four observed six-byte values verbatim, ranks each
chunk by its exact value and verifies **both LBA and sequence** before returning
read data. Sequence zero is valid: only `lba == NONE` / the missing map entry
means unmapped. This removes the old first-sequence-zero omission as well.
These changes improve recovery reads; they do not establish a complete FTL,
TRIM handling, sequence rollover semantics, or an authoritative live snapshot
while another OS/controller writer is active.

The new default cache is `/var/lib/iosnand/pages.v2`, magic `IOSNAND2`. Its
40-byte little-endian header declares die/block/page counts, record size, payload length and
an IEEE CRC32 covering all block and page records. Exact file length, geometry,
record bounds and checksum are checked before map construction. CRC32 detects
accidental corruption; it is not authentication or proof of NAND freshness.
A corrupt recognized v2 is discarded and rescanned. The old v1 omitted the
individual values and cannot be converted reliably: it is retained untouched
and a new scan creates v2 separately. An explicit `IOSNAND_CACHE` pointing at
v1 or an unknown file is rejected **by header identity, before any backend is
opened**. Symlinks, FIFOs, directories and unreadable existing paths are also
rejected. Cache saves use private 0600 unique temporary files, flush/fsync and
atomic rename; failed saves leave the previous cache intact and clean their
own temporary. Publication checks reject a foreign existing destination too;
cooperative tools take the bus locks, but this is not a hostile-path-race API.

The page table grows from 26,148,864 to 43,581,440 bytes: **16.625 MiB additional
RAM** on a 512 MiB iPad. Map construction now compares the winner's sequence
directly in that immutable page table instead of allocating a duplicate
`uint64_t best[NLBA]`. This saves **31,250,000 bytes / 29.802 MiB peak RAM**;
the 15,625,000-byte L2P table remains. Valid sequence zero, first-wins ties and
full u48 comparisons are preserved. A separate 68-LBA oracle and 544 generated
candidates check the actual map builder without using its sequence decoder
to calculate the expected result. Caches remain ordinary Linux filesystem
files; no NAND programming is added.

### Explicit read-only MTD backend (`tools/nand/iosnand-mtd.h`)

`IOSNAND_BACKEND=mtd` now selects a kernel-mediated read path for `iosnand`.
The default remains `pio` for comparison. The backend discovers exactly named
`apple-nand0` through `apple-nand3` dies in sysfs, opens the corresponding MTD
character devices **O_RDONLY**, and refuses wrong page/OOB/erase geometry,
writeable devices or missing dies. It never falls back to `/dev/mem`.
The existing two userspace bus locks are also taken to exclude cooperating
legacy tools; kernel MTD readers serialize through the kernel driver's mutex.

`MEMREAD` fetches 16 KiB data plus 64 bytes OOB at a 64-bit die-relative offset;
the adapter reconstructs the PPN's four `(1024 data, 16 metadata, 3072 data)`
chunks. `MEMGETINFO.size` is only 32 bits and truncates a 4.46 GB die, so the
64-bit sysfs size is checked as well. This check verifies the driver's declared
geometry, not the physical vendor/capacity of an untested iPad.

The mapping follows our driver and the exact
[v6.12 MEMREAD implementation](https://github.com/torvalds/linux/blob/v6.12/drivers/mtd/mtdchar.c):
success becomes readable status 0x40, `EUCLEAN` or corrected/max-bitflip stats
become 0x42. `EBADMSG` becomes retired status 0x45 **only** if `MEMGETBADBLOCK`
returns 1. An unretired ECC failure aborts; if the bad-block query itself fails,
both read/query errors are logged and the query error is propagated. Short
data/OOB returns and uncorrectable stats fail before caller bytes are copied.

MTD does **not** preserve physical PPN erased status 0x49. The adapter therefore
does not label all-FF data erased or stop early on it. MTD mode scans all 256
pages and bypasses the PIO incremental-cache shortcut on every run. This can
be materially slower; optimizing it requires a proven additional kernel API
or another freshness scheme. All-FF metadata remains unmapped, rather than
being promoted to a live FTL page. Retired blocks still skip safely.

Only `MEMGETINFO`, `MEMREAD` and `MEMGETBADBLOCK` ioctls appear in this backend;
there is no write/erase/feature operation. No MTD device was opened to test it.
The future opt-in command reads hardware and should only be used after device
boot approval and capacity verification; it is not part of these offline tests.

Ubuntu 22.04's ARM cross-sysroot predates the `MEMREAD` UAPI; the first ARM build
attempt exposed that missing header definition. The backend now carries only
the exact v6.12 request/ECC structure layout and ioctl number under its own
names, with provenance and the UAPI SPDX exception. Static assertions verify
the 64-byte request and mode/ECC offsets; modern native headers additionally
must match their real `MEMREAD`, size and ECC offset. No system header or Docker
image is changed. Runtime `ENOTTY` aborts rather than choosing legacy OOB reads
or `/dev/mem`. A native legacy-header simulation and fake runtime rejection
test cover both cases. The first attempt's log is
`../logs/night/iosnand-arm-build.log`; final ARM rebuild results belong in the
main build report.

### Bind an MTD node to its exact sysfs device number

A strict fake-FS regression of the original production `mtd_backend_open`
accepted `/dev/mtd0` and `/dev/mtd1` with their character-device minors swapped:
both still reported major 90, the same fixed geometry and read-only flags.
That selected the wrong physical die for the sysfs `apple-nandN` label; geometry
and name checks alone did not bind the opened node to the chosen kernel MTD.
No real device was opened or read for this reproduction.

The exact pinned Linux v6.12
[`mtdcore` MTD_DEVT and registration](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/mtd/mtdcore.c#L87)
assign `dev.devt = MKDEV(MTD_CHAR_MAJOR, index*2)`; `MTD_CHAR_MAJOR` is 90.
[`mtdchar_open`](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/mtd/mtdchar.c#L45)
selects the actual MTD with `iminor(inode)>>1`, independent of the filename.
The backend now reads the selected sysfs `dev` attribute, strictly parses its
unsigned major:minor fields with overflow/round-trip checks, and requires
`fstat(opened_node).st_rdev` to equal that exact device number. A missing,
malformed, wrong-major or mismatched device is refused; there is no redirection
to another node or fallback to PIO. Existing geometry/type/read-only checks
remain required. This binds discovery to the actual node, without claiming
a lease against subsequent privileged kernel/device reconfiguration.

`tests/test-iosnand-mtd-open.py` exercises **34 production discovery/binding
cases twice**, under modern and simulated pre-MEMREAD headers, with strict fake
directory/node/stat/ioctl calls and in-process memory streams for sysfs text.
Correct four-die mapping succeeds; swapped minors, wrong major, noncharacter
nodes, writable/SLC-emulated flags, legacy SLC/NOR types, duplicate names and
missing/malformed/overflowing dev attributes fail and close every opened fake
FD. Character/rdev mismatch reports
ENODEV, invalid geometry/text EINVAL, and actual stat/ioctl/read/close errors
survive cleanup instead of misleading "Success" diagnostics. Missing dies,
malformed size text and truncated attributes are also covered. No sysfs/device file or real
ioctl is used. The 29 layout/error/geometry cases pass under both headers.

The read-only boundary is software-defined: the H2FMI driver sets only
MTD_NO_ERASE, has no MTD_WRITEABLE flag, and registers read_oob/block_isbad,
with no write, erase or block_markbad callbacks. The v6.12 MTD open path refuses
write opens; core
[`erase`](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/mtd/mtdcore.c#L1375),
[`write_oob`](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/mtd/mtdcore.c#L1742)
and markbad paths check flags/callback availability. Its hardware-command
allowlist contains only reset/status/read/end-of-read opcodes. MEMGETBADBLOCK
uses a status read, not a markbad operation. The userspace MTD adapter issues
only MEMGETINFO, MEMREAD and MEMGETBADBLOCK, all on O_RDONLY descriptors.
This does not prove that opaque PPN firmware never performs internal
maintenance in response to a read; that requires hardware documentation and
validation. PIO tools remain a separate /dev/mem interface whose cooperative
lock does not arbitrate arbitrary MTD users, and are unchanged by this fix.
No writable callback, opcode, permission or PPN feature-state function was added.

### Kernel MEMREAD progress, ECC reporting and cleanup errors

A second offline review found a real cross-layer liveness bug. The H2FMI
`_read_oob` path previously returned `-EBADMSG` before incrementing either
`ops.retlen` or `ops.oobretlen` on retired/other unreadable pages. Linux v6.12
[`mtdchar_read_ioctl`](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/mtd/mtdchar.c#L758)
allows ECC errors and loops until those lengths advance. A consistently bad
page therefore produced repeated reads of the same address with no progress.
A harness compiling the **actual driver, mtdcore wrapper and mtdchar loop**
reproduced two identical attempts for statuses 0x45 and 0x41; an offline
watchdog returned `ELOOP` on attempt two to avoid hanging the test.

The fixed driver consumes a physically completed page's requested data/OOB,
counts its failure in the request and returns a sticky `-EBADMSG` even when
later pages are good. This matches the MTD progress contract; unreadable
bytes are explicitly marked failed. Transport/PIO/status/end-command failures
still abort immediately and do not split an old raw page into caller buffers.
`iosnand`'s actual MTD adapter was tested through this same emulated MEMREAD
path: unreadable page bytes never reach its caller; a proven retired block can
be skipped, and corrected/refresh pages remain readable.

The end-of-operation command previously discarded its timeout result after
otherwise successful reads. It now propagates that failure and preserves an
earlier transport error when both fail; chip deselection and controller-state
cleanup still execute. Per-die diagnostic counters use a small lock, and the
request retains failures, refresh counts and its maximum refresh marker across
mixed pages. Status 0x42 supplies only the existing **minimal sentinel 1**,
not an observed exact number of corrected bits. No write, erase, refresh
programming or new opcode is introduced.

Remaining code-review limits: individual hardware polls are bounded but use
zero-delay busy loops (100 ms for command/PIO, 2 s for status); successful
cleanup does not prove an electrically wedged bus recovered. Kernel bus mutexes
cannot serialize unrelated `/dev/mem` clients. The driver still relies on
inherited iBoot timing/clock state and fixed upstream geometry. These remain
hardware/architecture questions; no raw recovery sequence was executed here.

## RAM and scan-time budget on a 512 MiB iPad

These are source/build measurements and explicitly labeled extrapolations,
not measurements from the attached iPad. The tools still target upstream's
fixed 16 GB geometry; our unit's actual storage capacity has not been read.
This path uses **NBD, not FUSE** (`CONFIG_BLK_DEV_NBD=y`; FUSE is disabled in
the built configuration).

| Item | Bytes / MiB | Lifetime and evidence |
| --- | --- | --- |
| Exact page records | 43,581,440 / 41.563 | `4 * 1064 * 256 * sizeof(page_rec)`; resident while serving |
| Logical-to-physical map | 15,625,000 / 14.901 | `NLBA * sizeof(uint32_t)`; resident while serving |
| Removed duplicate winner table | 31,250,000 / 29.802 | Previously allocated during map construction; now absent |
| Existing ARM static ELF text/data/BSS | 1,696,776 / 1.618 | `size build/night-tools/iosnand-v2-cache`, before the small map optimization; includes global raw-page/cache/block/NBD buffers and libc |
| Cache v2 file | 43,649,576 / 41.627 | 40-byte header + block records + page records; RAM-backed files consume separate memory |
| Two cache files during atomic save | 87,299,152 / 83.255 | Existing cache plus new temporary until publication; relevant on a RAM root |

The source-accounted process baseline falls from **87.884 MiB at map-building
peak to about 58.082 MiB**, before stacks, allocator bookkeeping, kernel queues
and filesystem page cache. Serving does not release the page or L2P tables.
The NBD workers fork after mapping: these immutable tables are initially
shared through copy-on-write, so summing each worker's RSS overcounts physical
memory. Worker page tables/private changes and filesystem read caching still
add memory. Valid NBD reads larger than the 1 MiB static buffer still allocate
their payload. The pinned kernel's NBD queue advertises up to 65,536 sectors
(32 MiB), so even normal I/O can add a sizable transient buffer; no new
artificial request-size limit is introduced.

MTD adds two kernel bus raw buffers totaling 32,896 bytes. An aligned MEMREAD
also has 16,448 bytes of kernel data/OOB bounce storage; the userspace adapter
uses approximately 20 KiB of stack arrays/request state. These are small
compared with the map. This accounting does not estimate slab, network queues,
HFS+ caches or the eventual Wi-Fi/graphics workload.

The checked local Device Tree advertises **502.984 MiB** of RAM at
`0x80000000`; the 4 MiB framebuffer begins beyond that range and must not be
subtracted twice. A separate 4 KiB reset page is reserved inside it. Boot-time
patches may change the effective ranges. The kernel config selects 64 MiB CMA,
but movable allocations can occupy CMA memory: it is not automatically a
64 MiB private loss. Swap support is compiled in, without an activated swap
device in this workflow.

The current build's unpacked initramfs tree occupies about 36 MiB on the host
filesystem. That is a rough file-footprint estimate, not an observed device
RSS. `readelf -sW build/linux/vmlinux` gives a 44.718 MiB `_text` to `_end`
range including alignment and init memory; the 18 MiB `__init_begin` to
`__init_end` range is normally reclaimed after initialization. A rough sum of
the retained kernel range, tree, process tables and one RAM-backed cache is
about **163 MiB**, or **190 MiB while replacing an existing cache** before
building the map. Some file/executable pages overlap in that simple sum.
Dynamic kernel/process allocations, page cache, boot overlap and other
workloads are omitted; it is not a guarantee of safe headroom. With init memory
still retained, add another 18 MiB to these examples. RAM-root cache files
and recovered filesystem images cannot be treated as free disk space.

Before any separately approved device test, the practical acceptance metrics
are real `MemTotal`, `MemAvailable`, process RSS/PSS, CMA availability and OOM
logs during scan/cache-save/map-build/serve. Do not perform that test here.
Keep the Python dump reconstructor and multi-gigabyte recovered images on
the Firebat: on 64-bit Python its two NLBA-sized lists alone need about
59.6 MiB of pointers, and distinct integer objects can push a full mapping
to roughly 300 MiB before input/output buffers.

### Full scans: historical timing is not an MTD result

The [upstream NAND notes](p105-nand.md) record an earlier **714-second PIO
metadata scan** on an iOS 8.4.1 unit: 2,944 data blocks, 1,277 erased and 31
retired. The same notes report 14.6 MB/s PIO into tmpfs. These are upstream
measurements, not local device logs. Cached v1 PIO's old one-second result
does not measure the larger, checksum-validated v2 cache or MTD.

Current MTD cannot obtain the PPN `0x49` block-head status through ordinary
MTD OOB and therefore conservatively scans all 256 pages, even when a page is
all-FF; known retired blocks can still stop early. At fixed geometry, 4,252
non-boot blocks imply up to **1,088,512 full 16,448-byte page reads**, about
17.90 GB. Dividing by the historical 14.6 MB/s yields **20.4 minutes of
illustrative raw transfer time**. Different hardware, ioctl/copy/CRC/cache
costs and retired-block handling can change this materially; this is neither
a measured duration nor a guaranteed lower bound. Avoid a short scan watchdog
based on cached PIO timings.

Cache load/save also transfers about 44 MB each. The upstream notes' roughly
5.5 MB/s NFS writes and 7 MB/s reads would add seconds per cache operation,
but network write buffering means those numbers do not establish fsync
completion time. `ios` now reports the selected backend and `IOSNAND_CACHE`
path, identifies every MTD start as a full scan and avoids promising a few
seconds merely because a cache file exists. Its start/stop/mount operations
are unchanged; shell syntax and isolated announcement fixtures were checked.

Read recovery remains distinct from Linux-owned storage: a read-only System
NBD device neither creates a writable Linux root nor decrypts iOS-protected
Data files. RAM root, an approved NFS root or future proven USB storage can
keep Linux state without NAND writes. The separate writable PPN/FTL design
and its destructive boot/recovery consequences above remain unresolved and
unauthorized.

### NBD bounds before memory allocation

The actual `serve_one` handler previously called `malloc(len)` for a large
READ before validating its partition range. A local socketpair/malloc-wrap
fixture reproduced an invalid `from=UINT64_MAX, len=UINT32_MAX` request
attempting a 4 GiB allocation and returning `ENOMEM` instead of immediate
`EIO`; the wrapper refused that allocation, so no large buffer was created.
The handler now checks the existing subtraction-based bounds first. Valid
large reads retain their existing allocation and out-of-memory behavior.

The actual production handler passes 25 ASan/UBSan socketpair cases: overflow
and boundary failures without allocation; valid zero, small, unaligned and
large reads; simulated allocation failure; write/FUA-write refusal with fixed
buffer draining; TRIM, unadvertised WRITE_ZEROES and unknown-command refusal;
flush with no writable state; disconnect, invalid/truncated headers and
truncated nominal 4 GiB writes. A WRITE followed by FLUSH proves the rejected
payload is drained exactly and the next request retains its framing. Read
fixtures use unmapped zero-filled LBAs; any PPN or MTD operation aborts the
test. Only process-local Unix sockets are opened, without kernel NBD attach,
real ioctl, device access or writable storage.

### NBD setup ownership, worker lifetime and failure cleanup

The earlier `attach` path reset `/dev/nbdN` unconditionally before configuring
it, leaked setup descriptors on socket/ioctl failures and treated a failed
`fork()` as success. That negative child PID could reach `waitpid(-1)` later.
A failure starting a second export did not unwind the first explicitly, and
later DO_IT workers inherited older export peers and NAND descriptors/locks.
Seven baseline cases reproduced these failures with every relevant API faked.

Setup now issues **SET_SOCK before geometry or flags**, without an initial
CLEAR. The pinned kernel's
[`nbd_add_socket`](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/block/nbd.c#L1169)
checks the legacy setup task and Netlink-bound state in that operation.
This avoids the previous destructive reset of an already owned export; it
does not create a permanent ownership lease against concurrent privileged
device-control clients. A failed SET_SOCK closes only this process's FDs.
Before any DO_IT child could run, setup failures can unwind this process's
successful SET_SOCK with CLEAR. Once a positive child exists, cleanup uses
**only this process's socket/control descriptors and targeted child waits**,
without global CLEAR or DISCONNECT ioctls. In ordinary isolated operation,
final kernel configuration references release their resources.

The common unwind closes every server peer before waiting for positive own
child PIDs, retries interrupted waits and reports child exit, wait and close
failures. Partial multi-export startup uses it too. Linux close errors are
reported without retrying a descriptor that may already have been released.
An early failed worker need not release the kernel socket reference, so the
serve loop also monitors own children with WNOHANG instead of depending solely
on socket HUP. Early exit or fatal poll failure aborts and unwinds the session.

Each DO_IT child enumerates `/proc/self/fd` and closes every inherited
descriptor except its own NBD control fd and stderr: previous export peers,
NAND descriptors and their inherited lock references do not remain alive.
Enumeration/close/DO_IT failures produce a nonzero exit, with diagnostics in
the existing log. The child does not reset the device on exit. `dirent.h` and
`limits.h` are explicit source dependencies; no extra package is installed.
The target initramfs already mounts procfs; missing/unreadable procfs aborts
that worker rather than falling back to retaining unknown descriptors.

Kernel review limits are explicit. In this v6.12 source, `nbd_bdev_reset` only
adjusts capacity; `task_setup=NULL` occurs in `nbd_clear_sock` and the Netlink
disconnect path. Parent FDs/local flags are therefore not claimed as universal
generation tokens. No automatic reset of stale or foreign devices is added.
Concurrent privileged external reconfiguration, stale sysfs capacity after
abnormal teardown, the existing startup signal-handler window and a truly
wedged kernel wait still require hardware/process integration assessment.
There is no automatic signal escalation or timeout-based device reset.

The actual production functions pass **36 ASan/UBSan fake-API cases** covering
all SET failures, foreign EBUSY, failed fork, setup-close failures, complete
and partial startup unwind, positive-only waits, EINTR, child/cleanup errors,
worker exit/poll detection, post-running foreign takeover without reset and
post-fork FD removal. `open`, socketpair, ioctl, fork, exit, wait, signal,
poll (including fortified `__poll_chk`) and directory APIs are intercepted;
unreachable request read/write or unexpected kill abort. No real process fork,
device access, kernel NBD attachment or signal is used. These fixtures test
userspace control flow; they do not emulate every kernel configuration-ref
or hardware behavior.

## Private daemon registration, pidfd control and export readiness

The four fake-kill baseline fixtures demonstrated that the old wrapper could
signal a reused PID (including malformed group-like PID text). That numeric
PID control has now been removed. The target-only `iosnandctl` helper and
shared `iosnand-process.h` use Linux pidfds, never `kill(PID)` as a fallback.
Do not run this target helper on the production Firebat. No real process has
been signaled, registered or stopped during this work.

`iosnand --pid-record /run/ios/pid serve System [Data]` validates its CLI and
backend selection, then registers itself **after exec, before allocation,
cache validation, any NAND open or scan**. It opens its own pidfd before
checking proc identity and matches the actual executable inode/device against
root-owned `/sbin/iosnand`. Record errors abort before either NAND backend.
The shell no longer writes `$!`; it waits up to twenty half-second intervals
for registration. That bounded wait covers startup registration, not a
full NAND scan. Manual `iosnand serve` remains available without registration.
`--pid-record` is refused for scan/parts/read and malformed arguments.

A primary record is exact bounded decimal text:
`IOSNANDPID1 PID STARTTIME EXE_DEVICE EXE_INODE\n`.
PID 0, PID 1, negative/group-like values, overflow, legacy plain-PID files,
truncation, unexpected fields and embedded NUL bytes are refused. The final
parent directory must be root-owned mode 0700, pinned with an open directory
FD; records must be regular root-owned mode 0600 with one link. The final
parent/record components are opened without following symlinks and records
with O_NONBLOCK, so FIFO input cannot hang control. Intermediate path
components and other root processes remain trusted; this is not an ownership
lease against privileged file replacement.

Publication writes/fsyncs an exclusive private temporary file, links it to a
new final name without overwrite, removes only the temporary file it created,
and fsyncs the directory. Partial I/O/close/fsync failures may retain a
published record, but never permit an unregistered backend start. `clean`
removes only a valid expired primary record: pidfd ESRCH or an exact
starttime/executable mismatch indicates expiration. Permission, read,
unsupported-syscall, poll-error and unknown-format failures retain the file.
An active record is refused with EBUSY. Publishing and cleaning use an
exclusive directory flock to serialize cooperative clients; other root
processes can still interfere. The wrapper does not chmod existing runtime
directories, blindly remove legacy PID files, or escalate signals.

For status/stop, pidfd_open occurs **before** proc identity checks. A pinned
proc directory supplies field 22's starttime (comm parentheses/spaces are
handled), executable inode/device, root process ownership and the serve
command line. The bound pidfd is checked for exit both before and after these
checks. Stop uses only pidfd_send_signal(SIGTERM) on that bound descriptor.
A process exiting or its numeric PID being reused cannot redirect the signal
at the replacement. Missing pidfd syscall definitions fail compilation;
ENOSYS at runtime refuses control rather than falling back. POLLNVAL/POLLERR
are preserved as errors and cannot classify an active record as expired.
Before the serve-loop handler is installed, SIGTERM has its normal terminating
behavior; a scan can be stopped without a NAND program/erase operation.

Registration does **not** authorize mounting a globally positive nbd0 size.
After all own exports attach, the daemon checks each kernel NBD worker PID,
exact configured capacity and read-only flag, while monitoring early child
failure. Missing pid attributes or an own PID with still-unmatched size/RO (EAGAIN)
get at most ten poll cycles (each poll has a
one-second timeout); failure or timeout unwinds only owned process FDs and
children. No stale/foreign export is reset. The pinned v6.12 kernel exposes
`pid` during DO_IT startup after parsing the read-only flag; SET_SIZE_BLOCKS
has already configured the capacity before fork, and startup sets it again.
See [`nbd_start_device`](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/block/nbd.c#L1397)
and [`__nbd_ioctl`](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/block/nbd.c#L1506).

Only then does registration publish a generation-specific private file
`pid.ready.DAEMON_PID.STARTTIME` containing
`IOSNANDREADY1 WORKER_PID STARTTIME EXE_DEVICE EXE_INODE EXPECTED_SECTORS\n`.
Sectors equal `floor(partition_bytes / 4096) * 8`, exactly the existing NBD
SET_SIZE_BLOCKS setup. `iosnandctl ready` binds both the current daemon and
that generation's first worker with pidfds, verifies matching executable
identity, rereads nbd0 kernel PID/size/ro=1 and checks both pidfds for exit.
The wrapper mounts only after this check. Registered readiness requires
System as export zero; this API confirms **System/nbd0**, not continuing Data
ownership or a permanent lease against concurrent privileged NBD clients.
Checks use canonical `/sys/devices/virtual/block/nbdN` so O_NOFOLLOW does not
reject `/sys/block/nbdN`'s ordinary symlink. Generation files are retained
until the RAM-backed runtime directory disappears at reboot; primary-record
cleanup never deletes another generation's readiness file.

No userspace pre-mount check can exclude a privileged foreign reconfiguration
immediately afterward. Kernel stalls, actual process/sysfs integration,
concurrent privileged NBD administration and hardware behavior remain
unverified. Existing limits on cleanup waits still apply. No automatic
restore, disconnect/CLEAR after possible DO_IT, host configuration change or
NAND write has been introduced.

`tests/test-iosnand-process.py` runs **84 actual production-header/main
ASan/UBSan cases** with strict fake pidfd, proc/sysfs metadata, poll, record I/O,
publication/cleanup and process APIs. Cases include reuse before/after binding,
post-check death, ENOSYS, expired/active/legacy/private-file checks, short or
wrong commands, exact executable matching, atomic no-overwrite/error cleanup,
wrong/missing readiness generation, dead/reused worker, wrong/zero capacity,
RW state, actual own-worker retry/foreign rejection/bounded timeout, and registered CLI failures before allocation/backend access. PPN,
fork, ioctl, socketpair, wait and signal calls outside the fixtures abort.
`tests/test-ios-wrapper-process.py` runs **10 actual wrapper-function fixtures**
with an inert local helper and daemon, never real mounts/process signals:
foreign positive capacity, stale generation and failed/reused workers cannot
mount; own readiness permits System mount; safe stop/expiration removes only
expired fixture state while refused/slow/untrusted control retains it.

## Validation and limits

```sh
python3 tests/test-nand-ftl-scan.py -v
python3 tests/test-iosnand-parse.py
python3 tests/test-iosnand-cache.py
python3 tests/test-iosnand-mtd.py
python3 tests/test-iosnand-mtd-open.py
python3 tests/test-h2fmi-read.py
python3 tests/test-iosnand-nbd.py
python3 tests/test-iosnand-lifecycle.py
python3 tests/test-iosnand-process.py
python3 tests/test-ios-wrapper-process.py
```

Results: 22 Python binary-fixture/CLI tests, 36 C production-parser/bounds
cases, 95 production v2-cache/exact-sequence/map-oracle cases and 25 fake-ioctl MTD layout/error/geometry cases pass (the last group
runs with both modern and simulated pre-MEMREAD build headers). In addition,
21 actual H2FMI/MEMREAD/adapter integration cases pass. This harness imports
only the exact local v6.12 MTD UAPI header and production functions, so old
system headers are not replaced. Another 25 actual NBD socketpair cases
and 36 production lifecycle/fake-API cases pass. The new process/control
fixtures add 84 strict fake-API C cases and 10 fake-control shell cases. The C harnesses
run with AddressSanitizer and UndefinedBehaviorSanitizer, do not link `ppn.c`,
and abort if a test reaches any PPN open/claim/read/lock stub. The production
`scan_block` test finds a valid page after an all-FF page and reads all 256 pages.
No real ioctl or device open occurs. Fixtures
are synthetic; no personal data, actual iOS dump or hardware is required.

Not established: this device's NAND vendor/capacity beyond the model's upstream
geometry; behavior with a real 9.3.5 dump; actual kernel-driver hardware
reliability; writable PPN mode; UBI performance; host-mode USB storage.

## Persistence that preserves iOS

1. **NFS root from the Firebat:** already documented and supported upstream.
   Kernel/initramfs stays a local boot artifact; package/data state lives on
   the host. Host exports/routing still require separate authorization.
2. **USB drive after OTG is proven:** Linux can use a conventional filesystem
   on removable block storage. Requires working host-mode PHY, VBUS/source
   control and a suitable adapter/hub; the port no longer carries gadget SSH.
   Wi-Fi must first be stable. No USB drive was formatted or mounted here.
3. **RAM root plus SSH transfer:** immediately fits the present temporary boot
   model; build/log/results remain on the Firebat, with no persistent iPad disk.

The priority for working development is an external persistence path plus
stable charging/OTG. NAND research can improve read-only correctness now; a
writable iOS-replacing experiment remains a separate project decision with
different boot/recovery requirements and no authorization in this session.
