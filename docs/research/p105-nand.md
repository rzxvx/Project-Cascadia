# P105AP NAND — H2FMI + PPN, read-only bring-up

**Rule for everything below:** the NAND holds the iPad's iOS install.  Nothing
here programs or erases it.  `tools/nandctl.c` sends NAND opcodes only through
an allowlist (reset, read ID, status, PPN get-feature / device parameters /
data out); program (80/10), erase (60/D0) and PPN set-features (EF) are not in
it.

## Status (2026-09-27)

From Linux, over `/dev/mem`, both buses answer: read ID, PPN firmware version
and the PPN device-parameter page, all matching what iOS reports.  Page reads
work (PIO, ~14 MB/s through `nandctl dump` to the NFS root) and are
bit-for-bit repeatable; the flash partition table reads back and decodes.
No kernel driver yet, no FTL yet.

## What the NAND is

Two Hynix PPN packages, one per bus, each on CE0 (ADT `ce-bitmap 0x101`).
PPN = the package has its own controller: ECC and part of the bad-block work
happen inside it, the host talks a command protocol on top of Toggle-DDR.

| | value | source |
|---|---|---|
| PPN firmware | `040472P_HYNIX_B_` | feature 0x9080, iOS `firmware-version` |
| marketing name | `20nm64Gb` | iOS `nand-marketing-name` |
| read ID (addr 0) | `50 50 4E 01 05 05` = "PPN" v1.5.5 | ADT `device-readid`, Linux |
| manufacturer ID | `AD 82 52 23 21 02` | iOS |
| page | 16384 + 64 bytes spare (`0x4040`) | device params |
| pages / block | 256 MLC, 128 SLC | device params |
| blocks / CAU | 1064 (`0x428`) | device params |
| CAUs / CE | 2 | device params |
| bits | cau 4, block 11, page-address 8 | device params |
| raw size | 2 CE x 2 CAU x 1064 x 256 x 16 KB ≈ 17.9 GB | |

## iOS's storage stack (IORegistry)

`flash-controller0` (fmi,s5l8920x) → `AppleIOPFMI` (the IOP coprocessor
drives the FMI; iOS never does it from the AP) → `IOFlashStorageDevice disk`
→ `IOFlashPartitionScheme`: Boot Block, Bad Block Table, NVRAM, Firmware,
System Config, Effaceable, Diagnostic Data, Filesystem → on Filesystem
`AppleSwissPPNFTL` → disk0, 16 000 000 000 bytes → `LightweightVolumeManager`
→ GPT → `System` (disk0s1s1, 2.2 GB, HFS+, read-only) and `Data`
(disk0s1s2, 13.6 GB, HFS+, content-protected).

## iBEC has the whole read stack

`build/firmware/iBEC.dec` (base 0x9ff00000) drives the FMI from the AP and
carries every layer needed to reach a file: `drivers/apple/h2fmi/*`
(H2fmi_ppn.c, H2fmi_ppn_fil.c, fmiss_ppn.c), `WhimoryPPN/Core/FPart` (flash
partitions), `SVFL/s_vfl.c`, `SFTL/*` (s_read, s_cxt_load, L2V_*), `lib/fs/hfs`.
It is how iBoot loads `/System/Library/Caches/com.apple.kernelcaches/kernelcache`.
That is the reference for a Linux read path.

## Hardware

Per bus: FMI (DMA/PIO) at 0x31200000 / 0x31300000, FMC (the NAND bus) at
+0x40000, ECC at +0x80000 (ADT `reg`, six 4 KB windows).  AIC 0x21 / 0x22.
PMGR gates 0x3f1010c4/c8 (FMI0), 0x3f1010cc/d0 (FMI1), iBoot ids 0x2f-0x32;
iBoot leaves them on.  Block reset = bit 31 of 0x3f1010c4 (FMI0) /
0x3f1010cc (FMI1), pulsed.

| reg | name | notes |
|---|---|---|
| FMI+0x00 | CONFIG | 0 for read ID, 5 for PPN data out |
| FMI+0x04 | CONTROL | 3 = start PIO transfer, 6 = reset/stop |
| FMI+0x0c | STATUS | bit 1 transfer done (W1C) |
| FMI+0x10 | INT enable | 0x100 = FMC event |
| FMI+0x14 | DATA | PIO FIFO, a read pops it |
| FMI+0x1c | DMA status | bits 3-4: data ready |
| FMI+0x34 | PIO config | `bytes << 8 \| sectors` |
| FMC+0x00 | ON | 1 SDR, 5 DDR (iBEC leaves 5); 2 after reset |
| FMC+0x08 | IF_CTRL | bus timing |
| FMC+0x0c | CE_CTRL | `1 << ce` |
| FMC+0x10 | RW_CTRL | phases: 1 cmd1, 2 cmd2, 8 addr; 0x50 = poll status |
| FMC+0x14 | CMD | cmd1 \| cmd2 << 8 |
| FMC+0x18/1c | ADDR0/1 | address bytes, LSB first |
| FMC+0x20 | ADDRNUM | bytes - 1 |
| FMC+0x40 | INT mask | 0x20 = status match |
| FMC+0x44 | STATUS | phase-done bits (W1C), 0x20 status match |
| FMC+0x48 | NAND status | last status byte |
| FMC+0x4c | status mask | 0x4040 = wait for bit 6 |
| FMC+0x70/74/78 | Toggle/DDR | `00c40000 03020100 01011d0b` from iBEC; a block reset clears them |

## Command sequences (from iBEC, run from Linux)

- **Read ID** (`h2fmi_nand_read_id`): CE, CMD 90, ADDR0 = addr, ADDRNUM 0,
  RW_CTRL 9, wait FMC_STATUS 9; FMI CONFIG 0, PIO 0x801, CONTROL 3, wait
  FMI_STATUS 2, read 8 bytes.  In DDR the device repeats each ID byte.
- **PPN status** (`h2fmi_ppn_get_operation_status`): CMD `77 7D`, RW_CTRL 3;
  then status mask 0x4040, INT mask 0x20, RW_CTRL 0x50, wait for the match,
  byte from FMC+0x48.  0x40 = ready, no error.
- **PPN get feature** (`h2fmi_ppn_get_feature`): `EE <feature, 2 bytes> E7`
  (RW_CTRL 0xb), status, `7A`, data out by PIO, CONTROL 6, `77`.
  Feature 0x9080 = 16-byte firmware version.
- **PPN device parameters** (`h2fmi_ppn_get_device_params`): `92 <00> 97`,
  status, `7A`, 512 bytes: `"PPN Device Info"`, then u32s at 0x10: CAUs,
  cau bits, blocks/CAU, block bits, pages/block, SLC pages/block, page-address
  bits, bits-per-cell bits, default bits/cell, page size; timings at 0xa0 (tRC
  tREA tREH tRHOH tRHZ tRLOH tRP, -, tWC tWH tWP, in ns); queue sizes at 0xe0;
  tRST / tPURST / tSCE ms, tCERDY us at 0xf0.

- **Page read** (`fmiss_ppn_read_multi`, which queues this on the FMI's
  sequencer at FMI+0xc0000; done by hand here): `0A <row, 3 bytes> 37`
  (RW_CTRL 0xb), status, `7A`, data out, `77`.  Row = page | block << 8 |
  cau << 19 (| slc << 23).  A page comes out as 16384 data + 64 metadata
  bytes (`0x4040`); the PPN has already corrected it.  Status 0x40 = good,
  0x49 = erased (all FF).  (The sequencer also queues `07 <row> 37` for all
  but the last page of a multi-page read.)
- **Boot pages** (LLB, the flash partition table) use the same read but carry
  the FMI's own BCH: 3 x (512 data + 53 parity), `ECC_CONFIG` 0x1a8 = 424 bits,
  `FMI_CONFIG` 0xf5 = ECC strength 30 << 3 | 5 (openiBoot:
  `((ecc_bits & 0x1f) << 3) | 5`).  iBEC decodes them only after switching the
  PPN to SDR (`transitionWorldFromDDR`: PPN set-feature 0x180 = 1, FMC_ON 1)
  and back (0x180 = 0xa, DDR).  In DDR the FMI skips 54 bytes per sector
  instead of 53: sector n comes out shifted by n bytes.  `nandctl bootpage`
  reads raw and drops the parity instead (no correction).

## Flash partition table

Boot page 0 and 1 of block 0, on every bank (bank 0 = FMI0 CE0, bank 1 = FMI1
CE0; all four copies identical).  1536 bytes: `"ndrG"` header, spare/remap
lists (`"VgrA"`, `"sbus"`), device geometry at 0x200 (1, 2 banks, 2128
blocks/CE, 256 pages, 32 x 512 bytes, 64 spare, 6, 0xdeadcafe), and at 0x400
32 entries of 16 bytes: tag, start block, block count, flags.

| tag | start | blocks | flags | IORegistry |
|---|---|---|---|---|
| boot | 0 | 1 | 0x002 | Boot Block |
| plog | 4 | 1 | 0x108 | Effaceable |
| nvrm | 2 | 3 | 0x108 | NVRAM |
| firm | 2 | 1 | 0x308 | Firmware |
| fsys | 1 | 2127 | 0x006 | Filesystem (the FTL) |
| scfg / diag / fbbt | 0 | 1 | 0x001 | System Config / Diagnostic / BBT |

## Page layout and the FTL's metadata

A page comes off the bus as four 4 KB logical pages, each **1024 data, 16
metadata, 3072 data** (4112 bytes; HFS+ nodes found in the stream pin this
down: record offsets jump by 16 at logical 0x400 of every 4 KB).  The
metadata is plain -- not whitened, whatever the ADT's `metadata-whitening`
means:

| bytes | field |
|---|---|
| 0 | type: 1 user data, 2 FTL context (L2V spans, "weaved" into the data stream), FF erased |
| 1 | flags |
| 2-7 | u48 write sequence: +1 per 4 KB; +16 per page, the stripe runs over 4 dies (2 banks x 2 CAUs) |
| 8-11 | u32 LBA in 4 KB units (disk0 = 3 906 250 of them); context pages use LBAs above that |
| 12-15 | u32 sequence >> 16 |

So disk0 can be rebuilt without parsing the SFTL context: for each LBA the
copy with the highest sequence is the live one (`tools/nand-ftl-scan.py`).
TRIMmed LBAs come back stale, which the filesystem never looks at.

## disk0: LwVM, then HFSX

LBA 0 holds the LwVM header: type `6A9088CF-8AFD-630A-E351-E24887E0B98B`,
media size 16 000 000 000, two partition records at 0x200 (0x80 bytes each:
type, GUID, begin, end, attributes, UTF-16 name) -- System and Data, both HFS+
-- and at 0x800 a map of 1024 16 MB chunks, one u16 per physical chunk:
0xF000 the header itself, 0xF3FF unused, else partition << 12 | chunk.
System is 143 chunks (physical 1-143), Data 803.  System's volume header is
HFSX (case-sensitive, journaled), 4 KB blocks, 543 000 of them =
2 224 128 000 bytes, exactly iOS's `disk0s1s1`.  It is not encrypted.

## Proof: iOS's System partition, rebuilt from a raw dump

Full dump (2 buses x 2 CAUs x 1064 blocks, 17.9 GB, `nandctl dump`), map by
`nand-ftl-scan.py scan` (765 317 LBAs live), `part ... System` -> 2.2 GB image.
`fsck_hfs -n` passes catalog, extents, hierarchy, attributes and bitmap; the
volume is `Donner12H321.P105OS`, `SystemVersion.plist` says 8.4.1 (12H321),
and it mounts on a Mac (attach with `-shadow`, the journal wants replaying).
183 signed Mach-O files verify with `codesign -v` (page hashes), and of the
104 964 SHA-1 page hashes of the 432 MB `dyld_shared_cache_armv7` all match
but the first 8 -- the cache header, which TaiG's untether rewrote (6
mappings instead of 3, two pointing past the code signature).

## Trap: an absent CE wedges the bus

In DDR the chip clocks data out with DQS.  Reading data from a CE with no chip
behind it leaves the FMC waiting for edges forever: RW_CTRL stays set,
FMC_STATUS stays 0, every later command times out.  FMI CONTROL=6 and
toggling FMC_ON do not clear it.  What does, like iBEC's `h2fmi_device_reset`:
pulse the PMGR reset bit, then FMI CONTROL=6, FMC_ON=5 and the three Toggle
registers back.  `nandctl` now refuses CEs outside `ce-bitmap`.

## Trap: two processes on one bus

Two command sequences interleaved on one FMC (a dump running while another
`nandctl` read the same bus) wedge the FMC and leave the PPN answering 0x11 /
0x51 (general error).  A block reset (`recover`) clears the host side only.
A NAND reset (FF) clears the PPN but drops it to the async (SDR) interface --
read ID in SDR then reads a clean `50 50 4E 01 05 05` -- and loses the DDR
features iBEC set at init: setting the power state back to DDR (feature 0x180
= 0x0a, status 0x40) is not enough, status reads come back as junk (0xbf).
The clean way back is a reboot through iBEC.  `nandctl` now takes a per-bus
lock (`/tmp/nandctl-busN.lock`) for every command that drives a bus.

## Next

A kernel driver: H2FMI + PPN reads (DMA instead of PIO), a map built from the
page metadata (or from the SFTL context, to avoid a full scan), LwVM on top;
the System partition then mounts with Linux's hfsplus.  Data is encrypted
per file (content protection) and needs the AES engine.
