# P105 NAND: behavioural and on-media specification

Linux PPN NAND driver and Swiss FTL with write support, for the iPad mini 1
(P105AP / iPad2,5, A5), iOS 8.4.1 12H321 on the media.

This document is the only input needed to implement the driver. It states what
the hardware does, what is on the flash, what iOS accepts, and what Linux must
do. It carries **facts and required behaviour**, each with its evidence. It
contains no driver code and no pseudo-code of any existing driver.

Credits: the write path that first proved all of this possible, and the
observation that this package's on-media format is the same on the A5 as on
the iPhone 5, are the work of **crystal-lized** (iPhone 5 S5L8950X series,
October 2026). crystal-lized allows the work to be used and is to be credited
in whatever is built from this spec.

## 0. Evidence and notation

Every fact below ends with its evidence in square brackets.

| tag | meaning |
|---|---|
| `[iBEC 0x…]` | code or data at that address of the iOS 12H321 iBEC, decrypted image (`devices/a5/build/firmware/iBEC.dec`, 282624 bytes, SHA-256 `30c04005…78ec`, load base `0x9ff00000`, Thumb-2; disassembled with `objdump -D -b binary -m arm -M force-thumb --adjust-vma=0x9ff00000`) |
| `[KC …]` | iOS 12H321 kernelcache, Mach-O (`devices/a5/build/firmware/kernelcache.12H321.macho`, SHA-256 `db1ddc83…dd2c`): VA or file offset |
| `[W0C …]` | raw dump `w0c` of 2026-10-27 (on the Mac, `~/cascadia-root BACKUP/srv/nand/w0c`, taken before any FTL write), one nand page = 16448 bytes as it came off the bus, status byte per page in the `.st` sidecar. Location: `<bus>-cau<c> block <b> page <p>`, in `bus{N}-cau{C}.b{S}.bin` (S = 0, 500, 1000), at file offset `((b−S)·256 + p)·16448` |
| `[DEV date]` | a measurement on the actual iPad, recorded in the project notes on that date (`docs/research/p105-nand*.md`, `p105-data-resize-plan.md`) |
| `[EARLIER]` | behaviour of the earlier driver, stated here as a fact about what a working read did; proven by that driver's device results, not confirmed independently from a binary or a dump. Treated as §9 treats unverified items |
| `[EARLIER DRIVER]` | order, values and waits taken from the earlier driver for the path from probe to the end of the second page read only (§2.9), by the user's limited permission of 2026-11-09. To be confirmed on the device; §9 |
| `[THIS]` | established independently while writing this document, with fresh scripts over `[W0C]` and `[iBEC]`; the numbers are reproducible and listed in §8 so they can serve as acceptance values |

A statement that is known only from an earlier driver and could not be
confirmed from a binary, a dump or a measurement is **not** in the main text:
it is in §9 "Unverified".

Words used here (none of them is a driver identifier):

- **bus**: one FMI/FMC pair. There are two, bus 0 and bus 1.
- **CE**: chip enable; here every bus has one populated CE.
- **CAU**: the package's concurrent addressable unit (a die, 2 per CE).
- **bank**: one CAU seen by the FTL (bank = cau·2 + bus; §3.2).
- **block / page**: physical; 16 KiB page, 256 pages per MLC block.
- **LBA**: 4 KiB logical unit; 4 per page.
- **VBA**: FTL virtual address of one LBA; `superblock·4096 + offset` (§3.3).
- **superblock (SB)**: the set of physical blocks with one number, striped over
  the banks that have a usable block with that number.

## 1. Controller (FMI/FMC), PIO only

DMA is deliberately out of scope. The rewrite moves data by PIO only and
does not need the CDMA channels or the FMI sequencer. Everything below is
sufficient for PIO on the A5.

### 1.1 Addresses and clocks

| item | value | evidence |
|---|---|---|
| FMI windows | bus 0 at `0x31200000`, bus 1 at `0x31300000` | `[iBEC 0x9ff050ec, 0x9ff03964]` compare the running base against them |
| FMC window | FMI + `0x40000` | `[iBEC 0x9ff07bb6 literal 0x40014]`, all FMC accesses use base+0x4xxxx |
| ECC window | FMI + `0x80000` | `[iBEC 0x9ff07462 literal 0x8002c]`, ADT `reg`, recorded `[DEV 2026-09-27]` |
| interrupts | AIC `0x21` / `0x22` (polling is sufficient) | ADT, `[DEV 2026-09-27]` |
| PMGR gates | `0x3f1010c4`, `0x3f1010c8` (FMI0), `0x3f1010cc`, `0x3f1010d0` (FMI1); on when iBEC hands over | ADT and PMGR notes, `[DEV 2026-09-27]` |
| block reset | pulse bit 31 of `0x3f1010c4` (FMI0) / `0x3f1010cc` (FMI1) | `[DEV 2026-09-27]` |
| populated CEs | ADT `ce-bitmap = 0x101`: CE0 on bus 0 and CE0 on bus 1 (logical CE 0 and 8) | ADT, `[DEV 2026-09-27]` |

A package with no chip behind a CE wedges the bus (FMC waits forever for DQS):
never select a CE outside the bitmap; the only way out is the block reset of
§1.3 `[DEV 2026-09-27]`.

### 1.2 Registers used

Offsets from the FMI base (`F+`) or from the FMC base (`C+` = FMI + 0x40000).
Registers not listed are not needed for PIO. iBEC's NAND code touches no FMC offset other than
those listed here: `0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x40, 0x44, 0x48, 0x4c, 0x70, 0x74, 0x78`
plus `ON` `[THIS: census of the iBEC NAND code's FMC offsets]`.

| register | behaviour | evidence |
|---|---|---|
| `F+0x00` CONFIG | 0 for read-ID transfers; **5 for PPN data-out** | `[iBEC 0x9ff075ee]` writes 5 before data-out; read ID `[DEV 2026-09-27]` |
| `F+0x04` CONTROL | 3 = start PIO receive, 5 = start PIO transmit (host to NAND), **6 = reset / stop the FMI** | `[iBEC 0x9ff07610 (3), 0x9ff0764a (6)]`, 5 `[DEV 2026-09-27: set-feature TX]` |
| `F+0x0c` STATUS | bit 1 = transfer done, write-1-clear | `[DEV 2026-09-27]` |
| `F+0x10` INT enable | `0x100` = FMC event | `[iBEC 0x9ff0808a]` |
| `F+0x14` DATA | the PIO FIFO: each read pops a word, each write pushes one | `[iBEC 0x9ff07d18]` (DMA target = base+20), `[DEV 2026-09-27]` |
| `F+0x1c` DMA status | bits 3–4 = data ready to pop | `[DEV 2026-09-27]` |
| `F+0x34` PIO config | `(bytes << 8) & 0x7fc00 \| sectors` (one burst, `bytes` ≤ 1024, multiple of 4) | `[iBEC 0x9ff07602–0x9ff0760c]` |
| `C+0x00` ON | 1 = SDR, 5 = DDR; reads 2 straight after reset | `[iBEC 0x9ff050f8]`, `[DEV 2026-09-27]` |
| `C+0x08` IF_CTRL | bus timing, §1.4; bit `0x100000` is cleared before a status poll | layout `[iBEC 0x9ff03dc4–0x9ff03de8]`; bit `[iBEC 0x9ff08096]` |
| `C+0x0c` CE_CTRL | `1 << ce`, 0 to deselect | `[iBEC 0x9ff059d0–0x9ff059f0]` |
| `C+0x10` RW_CTRL | phases: 1 cmd1, 2 cmd2, 8 addr; `0x50` = poll status; 0 stops a poll | `[iBEC 0x9ff0789c, 0x9ff07a70, 0x9ff080bc, 0x9ff080ee]` |
| `C+0x14` CMD | `cmd1 \| cmd2 << 8` | `[iBEC 0x9ff0783c (0x7d77), 0x9ff07a74]` |
| `C+0x18/0x1c` ADDR0/1 | address bytes, little-endian | `[iBEC 0x9ff07aa0, 0x9ff07abc]` |
| `C+0x20` ADDRNUM | `(addr_bytes + 7) & 7` | `[iBEC 0x9ff07ac0]` |
| `C+0x24` DATANUM | 0 before a status poll | `[iBEC 0x9ff080b2]` |
| `C+0x40` INT mask | `0x20` = status match | `[iBEC 0x9ff080ba]` |
| `C+0x44` STATUS | phase-done bits (1, 2, 8), `0x20` status match; write-1-clear | `[iBEC 0x9ff0785c, 0x9ff07ae2, 0x9ff080c8]` |
| `C+0x48` NAND status | last status byte; read after a poll | `[iBEC 0x9ff080cc]` |
| `C+0x4c` status mask | `0x4040` = wait for bit 6 | `[iBEC 0x9ff080a2]` |
| `C+0x70/74/78` toggle | `00c40000 03020100 01011d0b` restored by iBEC after a block reset; only needed if DDR is used | `[iBEC 0x9ff05bec–0x9ff05bfa]`, values `[DEV 2026-09-27]` |

### 1.3 Reset and bring-up

Required sequence for a bus, in this order:

1. Pulse the block reset bit (§1.1), then `CONTROL = 6`, then `ON = 1` (SDR)
   `[iBEC 0x9ff05100–0x9ff0510e]`.
2. Restore the timing (§1.4): a block reset clears `IF_CTRL`, and every operation
   that does a block reset must put the working value back `[DEV 2026-10-27]`.
3. NAND reset, opcode `0xFF` on the CE, waited on as a command phase; iBEC's
   wrapper is at `[iBEC 0x9ff052cc]`. A NAND reset drops the package to the async
   (SDR) interface; there is no need to re-enter DDR for PIO, and the same
   reset is the only known way out of a general-error state `[DEV 2026-09-27]`.
   Re-entering DDR through feature `0x180` alone is not enough (status reads
   return junk `0xbf`) `[DEV 2026-09-27]`.
4. Set feature `0x180` to 2 (async) on each CE, once all buses are reset, then
   write `IF_CTRL` again `[iBEC 0x9ff095e4, 0x9ff08c0a]`, `[EARLIER]` for the order.
   No other feature is needed before array reads `[EARLIER]`.
5. Read ID (§1.5) and device parameters (§2.1), then confirm the values of §2.1.
6. Two independent command streams on one FMC corrupt each other and leave the
   package answering `0x11`/`0x51`; one bus must be driven by one thread at a time
   `[DEV 2026-09-27]`.

The boot chain leaves `IF_CTRL = 0` and DDR on both buses
`[DEV 2026-10-26]`. Boot-page reads with FMI ECC need SDR and are not needed by
this driver: pages of blocks owned by the FTL are always PPN pages.

### 1.4 Timing

`IF_CTRL` is five 4-bit fields:

| bits | field |
|---|---|
| 19:16 | `dc`, read data capture delay |
| 15:12 | `rs`, read setup |
| 11:8 | `rh`, read hold |
| 7:4 | `ws`, write setup |
| 3:0 | `wh`, write hold |

Layout `[iBEC 0x9ff03d8e–0x9ff03e2e]` (each field inserted at bit 16/12/8/4/0);
meaning of the fields from the device sweeps below.

**The working value is `0x23210`** (`dc 2, rs 3, rh 2, ws 1, wh 0`) `[DEV 2026-11-08]`.

- The read fields were swept on the device: data correct for `dc` from 0 up to
  `rh + 2`, at any `rs`; from `dc = rh + 3` every byte is one place late. The
  centre of the window is `dc = (rh + 2) / 2`, which with `rh 2` is `dc 2`. A value
  on the last good `dc` (`0x43241`) produced transient bit-6 flips (DQ6) in the
  first minutes after a boot, with status `0x40` on every read; `0x23241` removed
  them `[DEV 2026-10-27]`. Read clocks were not swept further than this; §9 lists it.
- The write fields were swept for write speed: `ws 1, wh 0` gives a page transfer of
  0.73 ms instead of 1.33 ms, and page programs of 1.23 ms total
  `[DEV 2026-11-08]`. Real FMI clock: about 114 MHz, not 300 MHz `[DEV 2026-11-08]`.
- Timings derived for a too-fast clock can read **consistently shifted** data: a
  read that repeats is not a read that is correct. Any timing change is tested
  by comparing data against a reference, not by re-reading `[DEV 2026-10-27]`.

### 1.5 Elementary sequences

All addresses little-endian. After each phase wait for the phase bits in
`C+0x44` and clear them.

- **Command only**: `CMD = op`, `RW_CTRL = 1`, wait bit 0 `[iBEC 0x9ff0789c]`.
- **Command + address + command**: `CMD = op1 | op2<<8`, `ADDR0/1`,
  `ADDRNUM`, `RW_CTRL = 0xb`, wait `0xb` `[iBEC 0x9ff07a70]`.
- **Command + address** (for program): `CMD = op`, addresses, `RW_CTRL = 9`,
  wait `9` `[iBEC 0x9ff07e70]`.
- **Status poll**: clear pending FMC events, `F+0x10 = 0x100`, clear `IF_CTRL`
  bit `0x100000`, `C+0x4c = 0x4040`, `C+0x24 = 0`, `C+0x40 = 0x20`,
  `RW_CTRL = 0x50`, wait for status match (bit 5 of `C+0x44`) with a timeout,
  read `C+0x48`, then `RW_CTRL = 0` `[iBEC 0x9ff0807c–0x9ff080ee]`.
- **Operation status**: `CMD = 0x7d77`, `RW_CTRL = 3`, wait 3, then the poll
  above, up to 2 s `[iBEC 0x9ff0829c]`. The poll runs from the FMC's own status match:
  iBEC waits for its interrupt, the device-tested dump tool polls bit 5 of `C+0x44`
  with `F+0x10 = 0x100` set and no interrupt line, so the line is not required
  `[DEV 2026-09-27]`. Issued with no operation outstanding on an idle package it
  returns `0x00` and leaves the package in general error until a full reset
  `[DEV 2026-11-09]`. Status `0x40` = ready, no error `[iBEC 0x9ff07834]`.
- **Read ID**: `CMD = 0x90`, addr byte `0`, `ADDRNUM = 0`, `RW_CTRL = 9`,
  `CONFIG = 0`, `PIO = 0x801`, `CONTROL = 3`, read 8 bytes; returns
  `50 50 4E 01 05 05` ("PPN" 1.5.5). In DDR each ID byte is repeated
  `[DEV 2026-09-27]`, `[iBEC string 0x9ff35ed3]`.
- **Receive**: `CONFIG = 5`, `PIO = (n << 8 & 0x7fc00) | 1` with `n` ≤ 1024 and
  a multiple of 4, `CONTROL = 3`, wait for ready, pop `n` bytes, repeat, then
  `CONTROL = 6` `[iBEC 0x9ff075e4–0x9ff0764c]`.
- **Transmit**: as receive with `CONTROL = 5` and pushes `[DEV 2026-09-27
  set-feature; whole page DEV 2026-10-27]`. Exact CONFIG value on transmit: §9.

### 1.6 Page layout on the bus (the A5 quirk)

One page moves as 16448 bytes = four units of **4112**: **1024 data, 16
metadata, 3072 data**. The metadata does not come after the data on this SoC by
PIO: it comes at offset 1024 of every 4 KiB logical page, in both directions.
A programme with the metadata placed at +1024 of each 4112-byte unit is the
order that reads back correctly `[DEV 2026-10-27, 42/42 program cycles]`; the
receive layout also holds in `[W0C]` (HFS+ node offsets jump by 16 at every
+1024 of each 4 KiB unit, `[DEV 2026-09-27]`).

The metadata is plain: it is neither whitened nor scrambled on the bus
`[W0C: same bytes on all reads]`. It is not metadata-aware at the FMI level: for
PIO the FTL must extract and insert the 16 bytes at each +1024.

## 2. PPN protocol

Addresses in brackets are the operations as iBEC issues them.

### 2.1 Device parameters

Feature/parameter read: `0x92 <1 addr byte 0> 0x97`, status poll, `0x7A`,
receive 512 bytes `[DEV 2026-09-27]`. Layout, confirmed against the values
below on this package (`[DEV 2026-09-27]`, field names validated at
`[iBEC 0x9ff08520–0x9ff08800]` by assertions):

| offset | field | this package |
|---|---|---|
| 0 | ASCII `PPN Device Info` | |
| 0x10 | CAUs per CE | 2 |
| +4 | cau address bits | 4 |
| +4 | blocks per CAU | 1064 (`0x428`) |
| +4 | block address bits | 11 |
| +4 | pages per MLC block | 256 |
| +4 | pages per SLC block | 128 |
| +4 | page address bits | 8 |
| +4 | bits-per-cell address bits | |
| +4 | default bits/cell | 2 |
| +4 | page size | 16384 |
| 0xa0 | tRC tREA tREH tRHOH tRHZ tRLOH tRP, reserved, tWC tWH tWP (ns) | |
| 0xe0 | queue sizes | |
| 0xf0 | tRST tPURST tSCE (ms), tCERDY (µs) | |

A valid package is identified by read-ID `PPN` 1.5.5 and PPN firmware `040472P_HYNIX_B_`
(feature `0x9080`, 16 bytes, §2.2; `[DEV 2026-09-27]`). Only this package needs to be
supported. A driver refuses other firmware until tested.

### 2.2 Features

**Get feature**: `0xEE <feature, 2 bytes, LSB first> 0xE7` (cmd-addr-cmd),
status poll must return `0x40`, `0x7A`, then one or more receives of ≤ 1024
bytes, `CONTROL = 6`, `0x77` `[iBEC 0x9ff07564–0x9ff07652]`.

**Set feature**: `0xEF <feature, 2 bytes>` (cmd+addr), the value by transmit,
`0xE7`, status `[iBEC 0x9ff076c0]`, shape `[DEV 2026-09-27]`.

| feature | meaning | evidence |
|---|---|---|
| `0x9080` | PPN firmware version, 16 bytes | `[DEV 2026-09-27]` |
| `0x84 \| cau<<8` | bad-block bitmap of that CAU, `blocks_per_cau/8` bytes, get | `[iBEC 0x9ff07b04–0x9ff07b60]` |
| `0x3080`, `0x3180`, `0x3280` | program error lists (failed, retire, pending), 512 bytes each, get | `[iBEC 0x9ff0b744–0x9ff0b7e8]` |
| `0x3380` | clears / acknowledges the lists, set | `[iBEC 0x9ff0b814]` |
| `0x0180` | power state: 1 low power, 2 async (SDR), `0xa` DDR. A NAND reset leaves the package in low power. iBEC writes the transition from low power to async as `0x10002` to its helper, and leaves DDR in two steps, `0xa0001` then `0x10002`. While the state is low power iBEC holds `IF_CTRL` at `0xffff`, and it installs the working value when the target state is not 1 | `[iBEC 0x9ff08c0a, 0x9ff08be6, 0x9ff08c60, 0x9ff04d36, 0x9ff04d5e, 0x9ff05bc2, 0x9ff08c28–0x9ff08c34]`; values 2 and `0xa` also `[DEV 2026-09-27, 2026-11-09]` |
| `0x0480`, `0x00a0`, `0x01a0` | optional signal configuration used by iBEC in DDR | `[iBEC 0x9ff08a4a, 0x9ff08a84, 0x9ff08abc]` |

### 2.3 Status bytes

| value | meaning | evidence |
|---|---|---|
| `0x40` | ready, no error | `[iBEC 0x9ff07834, 0x9ff09a78]`, `.st` histogram `[W0C]` |
| `0x42` | read OK, block should be refreshed | `[iBEC string 0x9ff37569]`, `[DEV 2026-10-27]` |
| `0x45` | retired; block must not be used | `[iBEC 0x9ff09a7c]` (erase), `.st` histogram `[W0C]` (2048 pages, e.g. sb775 bank 2) |
| `0x49` | erased, all FF (including metadata) | `.st` histogram `[W0C]`, `[DEV 2026-10-26]` |
| `0x50` | general error during a program, seen on a same-CE batch (§2.8) | `[DEV 2026-11-09]` |
| `0x61` | an **SLC-programmed** page read with an MLC row (bit 23 clear): data come back but the read is by the wrong mode | `.st` of blocks 1055–1063 `[W0C]` |
| bit `0x10` | general error: the package needs recovery (§2.7) | `[iBEC 0x9ff07d9c, 0x9ff0b620]` |
| bit `0x01` | operation failed (program error) | `[iBEC 0x9ff0b628]` |

Any byte other than those above is an error to be reported, never treated as
success `[DEV 2026-10-27]`.

### 2.4 Addresses

Row address, u32 LSB first (4 bytes on PPN ≥ 1.5.0; `[iBEC 0x9ff08502]`; used alone in a program and an erase, inside the full address of §2.5 in a read):

```
page | block << 8 | cau << 19 | (slc ? 1 << 23 : 0)
```

- field widths from §2.1 (8 + 11 bits): `[iBEC 0x9ff07b22–0x9ff07b30]` extracts the
  CAU from the row with shift `page_bits + block_bits` and the mask from `cau_bits`.
- SLC selected with bit 23: `[DEV 2026-10-27]` (SLC and MLC program cycles on the
  six reserved blocks all pass).
- A read with bit 23 clear of a page programmed as SLC returns status `0x61`
  with the data intact, at **MLC page index** `g(k)` for SLC page `k`:
  `g(0..3) = 0,1,2,3` and `g(2j) = 4j−2`, `g(2j+1) = 4j−1` for `j ≥ 1`
  (SLC pages 0..127 land at MLC indices 0,1,2,3,6,7,10,11,…) `[THIS: W0C VFL context
  blocks, statuses of blocks 1055–1063, all four banks]`. This matters only for
  reading dumps taken with MLC rows; the driver uses the SLC bit.

### 2.5 Read

Sequence of a whole-page read in SDR, one operation:

1. Software reset of the bus (§2.5.2), select the CE, clear pending events.
2. `0x0A <8-byte full address> 0x37` (cmd-addr-cmd). Full address = u16 byte
   count (16448 for a whole page), u16 byte column (0 for a whole page), u32 row,
   all LSB first. `0x0A` is the form that ends a multi-page queue, and a single
   page is such a queue of one; `0x07` leaves the package expecting another page
   and must not be used for a lone read. Width and opcode: `[iBEC 0x9ff084d6–0x9ff08508]`
   (row 4 bytes and full address 8 bytes for PPN ≥ 1.5.0, 7 for older),
   `[iBEC 0x9ff0a08a]` (the normal read path sends the full-address width to the
   package), `[iBEC 0x9ff0790c]` (opcodes `0x0A`, `0x37`). Order of the
   fields in the 8 bytes: `[EARLIER]`.
3. Operation status (`0x7d77` + poll): `0x40` good, `0x49` erased; other values as §2.3
   `[iBEC 0x9ff07834]`.
4. `0x7A`, then the transfer of §2.5.1.
5. `0x77` `[iBEC 0x9ff07650]`, deselect the CE, software reset (§2.5.2).

The package has corrected the page. On a status that is not `0x40`, the data
are still clocked out, since the package has committed to send them `[EARLIER]`.

`0x0A <row only> 0x37` also exists: iBEC uses it for boot pages, with `CONFIG = 0xF5`
and the FMI's ECC `[iBEC 0x9ff0790c–0x9ff07918]`. The dump tool read every page
with a row-only address and straight 1024-byte bursts, repeatedly and correctly, but
that was with the boot chain's DDR state left in place `[DEV 2026-09-27, 2026-10-27]`.
There is no evidence that a row-only read is usable for whole PPN pages in SDR, and
`[DEV 2026-11-09]` shows it is not sufficient there: one read succeeds, the next
operation on the CE gets no status. Use the full address.

#### 2.5.1 Moving the page

A page moves as four units of 4112 bytes, each with its own start. For every unit:

- clear pending events (§2.5.2);
- `CONFIG = 5`;
- `F+0x34 = (1028 << 8) | 4`: **four sectors of 1028 bytes** (bits 18:8 hold the
  sector bytes, bits 7:0 the sector count);
- `CONTROL = 3`, one start for the whole unit;
- four times: wait for `F+0x1c` bits 3–4 to become non-zero, then pop 1028 bytes
  (257 words) from `F+0x14`.

No wait on `F+0x0c` bit 1 is done on receive. The 4112 bytes of a unit are a plain
stream in the §1.6 order, and the sector boundaries do not line up with the
metadata: the 16 metadata bytes at +1024 straddle the first and second sector, so
extract them only after the unit has been assembled. `CONFIG = 5` means ECC bypass and a
32-cycle data burst, with no metadata declared; everything goes through the data FIFO.

Evidence: all of this list `[EARLIER]`, with the result proven by device reads
through it: whole-FTL reads correct, file-for-file against the IPSW `[DEV 2026-10-26]`,
PIO read speed 8 MiB/s `[DEV 2026-11-08]`. `CONFIG = 5` and start value 3 also
`[iBEC 0x9ff075ee, 0x9ff07610]` on feature data-out. The burst layout with one
start per ≤ 1024 bytes is what the feature path uses `[iBEC 0x9ff075e4–0x9ff07644]`;
it is **not** the layout of a whole-page read.

#### 2.5.2 Software reset and clearing events

Software reset of a bus: `CONTROL = 6`, `ON = 1`, `IF_CTRL =` the working value.
It involves no PMGR pulse and no NAND reset. It is done **before every operation**
on the bus (before selecting the CE) and **again after every operation** (after
deselecting). `[EARLIER]`, `[DEV 2026-10-27]` (a direct write to `IF_CTRL` "does
not last": every operation's reset writes it back). iBEC's direct paths end the
same way, deselect then a reset of the bus, there with the PMGR pulse included
`[iBEC 0x9ff079e8, 0x9ff079f6, 0x9ff077b0, 0x9ff077be, 0x9ff050e8–0x9ff0511c]`.

Clearing pending events, in this order: `C+0x40 = 0`, `F+0x10 = 0`, `C+0x44 = 0x31ffff`,
`F+0x0c = 0xf` `[iBEC 0x9ff05914–0x9ff0592c]`. iBEC does this right after selecting a CE,
before the command, and again at the start of every status poll
`[iBEC 0x9ff07584, 0x9ff08084]`.

Between reads nothing else is kept or needed. The full reset of §1.3 (PMGR pulse,
NAND reset, power state) is for bring-up and recovery only.

The interface is SDR, `ON = 1`, for all page transfers of a Linux driver: the driver's
probe drops the package to async and everything after it, including the timing
sweeps of §1.4 and all device results quoted here, ran in that state
`[DEV 2026-10-26, 2026-10-27, 2026-11-08]`. DDR was only the boot chain's state and
is not needed.

### 2.6 Program

One whole page, one PPN at a time:

`0x8A <row>` (cmd+addr), transmit 16448 bytes in the layout of §1.6, `0x17`, then
status: `0x79` + poll `[iBEC 0x9ff07d32 (0x8A), 0x9ff07d5a (0x17), 0x9ff07bb4
(0x79 + poll)]`. Status: bit `0x10` general error, `0x01` failed, else the page
is good. After a program failure iBEC reads the error lists of §2.2 for a
multi-page queue `[iBEC 0x9ff0b744–0x9ff0b81a]`; a driver that programs one page
per CE gets the result from the status alone.

- Pages of a block are programmed in **ascending page order**, each page once.
  Any write within a block goes to the next free page. `[DEV 2026-10-27]`,
  and all iOS pages observed in `[W0C]` follow weave order within an SB.
- The whole-page PIO shape (0x8A / data with inline metadata / 0x17) was proven on the
  device on the six reserved blocks and on FTL blocks `[DEV 2026-10-27,
  2026-10-29, 2026-11-07, 2026-11-08]`.

### 2.7 Erase and recovery

**Erase one block**: `0x6A <row with page = 0> 0xD7` (cmd-addr-cmd), wait with
`0x79` + status poll, then `0x77 0x7D` + status poll for the block's result, then
`0x77` `[iBEC 0x9ff07f7e–0x9ff08000]`. Result `0x40` = done, `0x45` = failed, the block
must not be used. Several blocks on one CE go through the same loop with opcode
`0x67` for all but the last and `0x6A` for the last `[iBEC 0x9ff07f7e–0x9ff07f90]`;
one at a time works `[DEV 2026-10-27]`.

**Recovery** from a general error: reset both channels and the buses, run the
init of §1.3 including the power state, then restart the failed operation
`[iBEC 0x9ff09594–0x9ff09634]` (reset-all, device resets, re-init, power-state
set to `0x10002` / `0x1000a`, device resets). A retried operation that fails
again means the operation is abandoned, never silently ignored. §9 lists what
has not been exercised.

### 2.8 Overlap

- **Across chip enables, programs may overlap.** One page programmed on CE0 and one
  on CE1 at the same time works, doubling throughput: 8–10 MiB/s
  sequential write against ~5 MiB/s without `[DEV 2026-11-08]`.
- **Two pages in one operation on one CE does not work.** Every variant tried
  returned general error `0x50`: `0x87` without confirm followed by `0x8A`/`0x17`,
  `0x87 0x17` + `0x8A 0x17`, with reset between, with status between, on SLC and MLC,
  two `0x8A 0x17` without wait `[DEV 2026-11-09]`. The driver programs one page per CE
  at a time. iBEC's own multi-page path uses `0x87` for all but the last page of a CE
  through the sequencer `[iBEC 0x9ff0b3f8–0x9ff0b48a]`; its exact semantics are not
  reproduced by the direct sequences above.

### 2.9 Order of bring-up and reads as the earlier driver ran it

Everything in this section is `[EARLIER DRIVER]` unless another tag is given: the order
of register writes and package commands from probe to the end of the second page read
on a CE, as a driver that read the whole FTL correctly on this iPad ran it, in SDR. It
is listed because §1.3 and §2.5 did not suffice to make a second read work
`[DEV 2026-11-09]`. It has no independent confirmation beyond the tags shown; §9 says
what confirms it.

Common to every step:

- **Command phases.** `C+0x14`, then the address registers when there are any (`ADDR0`, `ADDR1`
  only when more than 4 bytes, `ADDRNUM = bytes − 1`), then `RW_CTRL`. Wait for the phase
  bits in `C+0x44` up to 100 ms and write exactly those bits back to clear them.
- **Clearing events.** `C+0x40 = 0`, `F+0x10 = 0`, `C+0x44 = 0x31ffff`, and write-1-clear of
  bits 0, 1, 2, 3 and 8 in `F+0x0c`. Same values as `[iBEC 0x9ff05914–0x9ff0592c]` except that
  iBEC writes `0xf` to `F+0x0c`, and iBEC then enables `F+0x10 = 0x100` for its interrupt.
- **Operation status.** `C+0x14 = 0x7d77`, `RW_CTRL = 3`, wait bits 0 and 1 of `C+0x44`, clear
  them. Then: clear events; `IF_CTRL =` working value with bit `0x100000` clear; `C+0x4c = 0x4040`;
  `C+0x24 = 0`; `C+0x40 = 0x20`; `RW_CTRL = 0x50`. Wait up to 2 s for **bit 8 of `F+0x0c`**
  (with `F+0x10` left at 0, no interrupt line). Read `C+0x48`. Write `0x100` to `F+0x0c`,
  `RW_CTRL = 0`, write the working `IF_CTRL` back.
- **Software reset.** `CONTROL = 6`, `ON = 1`, `IF_CTRL =` the current value.
- **Receive start.** `CONFIG = 5` (0 for Read ID), then `F+0x34`, then `CONTROL = 3`. Before
  popping a sector, wait until bits 3–4 of `F+0x1c` are non-zero, up to 100 ms.

In order:

1. For each bus: software reset, with `IF_CTRL` 0 at this point. Then write 0 to `F+0x80014`
   (a register in the ECC window that §1.2 does not list; iBEC writes the same register during
   its init `[iBEC 0x9ff05822]`). No PMGR pulse: this device's DT describes no reset line
   for the controller, so §1.3's pulse was not part of this driver's bring-up on the iPad.
2. For each bus in turn, NAND reset: for each of the 16 chip-enable numbers, write `CE_CTRL`
   (`1 << (n % 8)` for the 8 numbers belonging to this bus, 0 for the others), wait 10 µs,
   send command `0xFF` alone, wait its phase bit, write `CE_CTRL = 0`. Then wait 50 ms and set
   `IF_CTRL = 0xffff` (`[iBEC 0x9ff05bc2]` also uses `0xffff` after reset).
3. Same bus, Read ID on each of the 16 numbers: select, clear events, `0x90` with one address
   byte 0 (`RW_CTRL = 9`), `CONFIG = 0`, `F+0x34 = (8 << 8) | 1`, `CONTROL = 3`, wait for
   bit 1 of `F+0x0c`, pop 8 bytes, deselect, software reset.
4. Device parameters, first CE only, at `IF_CTRL = 0xffff`: select, clear events,
   `0x92 <00> 0x97` (cmd-addr-cmd), operation status must be `0x40`, `0x7A`, one transfer
   of the parameter page (`F+0x34 = (512 << 8) | 1`), `0x77`, deselect. No software reset.
5. Power state, each CE, still at `0xffff`: select, `CONTROL = 6`, `0xEF` with the two
   feature bytes `80 01` (`RW_CTRL = 9`), clear events, `CONFIG = 5`, `F+0x34 = (4 << 8) | 1`,
   `CONTROL = 5`, wait `F+0x1c` bits 3–4, push the word `2`, wait for bit 1 of `F+0x0c` and
   clear it, `0xE7` alone, operation status must be `0x40`, `0x77`, `CONTROL = 6`, deselect,
   software reset.
6. Each bus: write the working timing to `IF_CTRL`; this is the value every later software
   reset puts back.
7. Firmware version, first CE: select, clear events, `0xEE <00 90> 0xE7` wait
   (cmd-addr-cmd), operation status, `0x7A`, then a transfer of 16 bytes
   (`F+0x34 = (16 << 8) | 1`), `CONTROL = 6`, `0x77`, deselect. No software reset. Feature
   data longer than 1024 bytes goes in chunks of up to 1024 bytes, one start per chunk.
8. Bad-block table, each CE, each CAU: as step 7 with feature `0x84 | cau << 8`, 133 bytes.
   These reads therefore always ran before the first page read: 1 firmware read and
   `2 CE × 2 CAU = 4` table reads.
9. First page read: as §2.5 and §2.5.1, in this order: software reset; select; clear
   events; `0x0A <8 bytes> 0x37`; operation status; `0x7A`; for each of the 4 units: clear
   events, `CONFIG = 5`, `F+0x34 = (1028 << 8) | 4`, `CONTROL = 3`, four sectors popped;
   `0x77`, with no `CONTROL = 6` before it; `CE_CTRL = 0`; software reset.
10. Second page read: exactly step 9 again. Nothing else reaches the package between one
    read's `0x77` and the next read's `0x0A`.

The driver's commands on a bus are serialised behind one lock.

## 3. Geometry and layering

### 3.1 Raw geometry

| | value | evidence |
|---|---|---|
| Buses × CE | 2 × 1 | `[DEV 2026-09-27]` |
| CAU/CE | 2 | §2.1 |
| Blocks/CAU | 1064 | §2.1 |
| Pages/block | 256 MLC, 128 SLC | §2.1 |
| Page | 16384 + 64 | §2.1 |
| LBAs per page | 4 | §1.6 |

### 3.2 Bank order

The FTL sees four banks in this order: `(bus 0, cau 0), (bus 1, cau 0), (bus 0, cau 1),
(bus 1, cau 1)`, so `bank = cau·2 + bus`. Stripe order is bank 0, 1, 2, 3: within
a superblock the weaves of consecutive LBAs of one page row run bank 0 → 1 → 2 → 3.
`[THIS: W0C sb519 page 0, four banks, weaves 0x162d1045.. +4 per bank]`.

### 3.3 VBA → physical

`VBA = sb · 4096 + o`. Bank-aligned superblocks have 4 banks and `o` in `0..4095`:

- `lba_in_page = o % 4`
- `bank_index = (o / 4) % width`, where `width` = number of banks with a usable
  block `sb`
- `page = (o / 4) / width`

so a VBA stripes **dense over the banks present**: a superblock with one bank
missing holds `width · 1024` LBAs and its VBAs run contiguously over the three
banks, not with a hole at the missing bank.
`[THIS: W0C sb775, sb991 have bank 2 retired; a 4-bank rule reads LBA 289444 at VBA
0x307000 + 13 (expected 289448), the dense rule reads 289448; sb775's BTOC parses with the dense
rule; 120000 random LBAs of the full map verified, §8]`.

`0x7fffff` is the in-memory "no mapping" sentinel; it never appears as an LBA
and must never appear in metadata `[DEV 2026-10-29]`.

### 3.4 FPart

Block 0 of CAU 0 on both buses, pages 0 and 1 (boot pages with FMI ECC, 3 × (512 + 53))
hold the flash partition table: `ndrG` magic at offset 0, `VgrA` and `sbus` lists,
geometry at `0x200`, and at `0x400` 32 entries of 16 bytes: tag (`boot`, `plog`,
`nvrm`, `firm`, `fsys`, `scfg`, `diag`, `fbbt`), start block, count, flags `[W0C
bus0-cau0 block 0 page 0: ndrG at +0, VgrA at +643, sbus at +913 of the raw page]`, `[DEV 2026-09-27]`. `fsys` (the
FTL) is `(1, 2127)` in units of blocks over the 2128-block device. The driver
needs FPart only to confirm the layout; it must not write block 0.
iOS rewrites block 0 of CAU 0 by itself at times: `ndrG` word `0x10` went `0x0f`
→ `0x11` between two dumps `[DEV 2026-10-27]`; do not treat changes there as Linux's
doing, and do not rewrite it.

### 3.5 SVFL context

Per bank, the VFL block-usage table: blocks **1062–1063** of every bank, SLC,
`[W0C bus0-cau0 block 1063]` is the live one, 1062 erased in this dump.

Metadata of a VFL context page (MLC-index `g(k)` as in §2.4):

| bytes | field | evidence |
|---|---|---|
| 0 | `0x20` | `[THIS: W0C block 1063 banks 0–3, 59 pages]` |
| 1–2 | section: `0xffff` = header, `0..3` = bank table n | same |
| 3 | 0 | same |
| 4–7 | u32 = `~position in the log` (bank table `n` of the latest generation is the page whose complement counts up the log) | same |
| 8–15 | 0 | same |

Header page (latest `0xffff` page of the log):

| offset | field | value | evidence |
|---|---|---|---|
| 0 | u32 | `0xffffffff` | `[W0C]` |
| 4 | u32 | `0x3b9aca` = 3906250 (reported LBAs) | `[W0C]` |
| 8 | u32 | `0x100000` | `[W0C]` |
| 12, 14 | u16, u16 | 1055, 1055 (superblocks) | `[W0C]` |
| 0x100 | (u16 block, u16 bank)×8 | context copies: 0x427 ×4 banks, 0x426 ×4 banks | `[W0C]` |
| 0x200 + 32·n | u32 | log position of bank table `n` (the latest copy) | `[W0C]` |

Bank table page: u16[1064] = per-block usage, then `0xffff` to the end of the page:

| value | meaning | evidence |
|---|---|---|
| `0x0000` | FTL block, part of superblock N | `[W0C]`, `[DEV 2026-10-26]` |
| `0xbffe` | bad (factory / retired) | `[W0C bank 2: 775, 991, plus 1056, 1057, 0]` |
| `0xbff0` | available, free | `[W0C]` |
| `0xbff1` | VFL context | `[W0C blocks 1062, 1063 per bank]` |
| `0xb002` | block zero (FPart) | `[W0C]`, `[DEV 2026-10-26]` |
| `0xb003` | NAND boot blocks | `[W0C blocks 1055, 1057–1060]`, `[DEV 2026-10-26]` |
| `0xc101` | signature block | `[W0C]`, `[DEV 2026-10-26]` |
| `0xc104`, `0xc105` | diagnostics, System Config | `[W0C]`, `[DEV 2026-10-26]` |

Rules:

- The superblock `N` is block `N` of every bank whose table has `0x0000` at `N`
  (§3.3). 1055 superblocks, numbers 0..1054, of which 0 has no blocks (its entries
  are `0xb002` or `0xbffe`) `[W0C]`.
- A driver **never writes** the VFL context or any block whose usage is not `0`
  `[DEV 2026-10-27]`. A failed program ends the session; there is no retire path
  in the rewrite.

Signature block: block with usage `0xc101`, bank 2 of this device. Data, u32 LE, at
offset 128: `0x77726d78` ('xrmw'), 0, 6, 1, 1, 0, 2, 2, 0x20000, 0, 1, 0, 16, 2
`[W0C bus0-cau1 block 1061 page 0]`; at offset 32: `1, 0x600`. A driver compares
this exact byte image with the signature it was built for, and refuses to write
if different.

## 4. SFTL on media

### 4.1 Page metadata

16 bytes per LBA. Common: byte 0 = type.

**Data LBA** (`type = 0x01` or `0x02`):

| bytes | field | evidence |
|---|---|---|
| 0 | `0x01` user data, `0x02` internal (hidden LBAs and DELETE tokens) | `[W0C sb1]`, `[THIS]` |
| 1 | flags: `0x00` or `0x01` (static: cold data moved by wear levelling), normally constant in an SB, one sampled SB mixed both | `[THIS]`, `[DEV 2026-10-27]` |
| 2–7 | 48-bit LE weave sequence | `[W0C]` |
| 8–11 | u32 LBA (0..3906265) or token (`0xffff0000` PAD, `0xffff0001` DELETE) | `[W0C]` |
| 12–15 | u32 user sequence | `[W0C]` |

Weave and user rules:

- There is one weave counter for all streams; it advances by 1 per programmed LBA
  and each LBA keeps the value it got. Inside an SB the weave is monotonic but not
  necessarily consecutive: other streams may have used numbers in between
  (seen as "gaps", e.g. sb2, sb4, sb5 with span > VBA count) `[THIS: W0C, 9 of 39
  sampled SBs have gaps]`, `[DEV 2026-10-27]`.
- A new user write carries `user = weave >> 16` taken at the moment of programming
  `[THIS: W0C]`, `[DEV 2026-10-29]`. Data moved by GC keeps the `user` it had.
- Padding of a partially written page: slots are written with LBA `0xffff0000`,
  type 1, flags 0, a fresh weave and `user = weave>>16`; never `0x7fffff`
  `[DEV 2026-10-29]` (iOS's own PAD slot observed then, type 1, flags 0, `user` = weave high).

**BTOC metadata** (`type = 0x1c`), the same on every BTOC VBA of the superblock:

| bytes | field | evidence |
|---|---|---|
| 0 | `0x1c` | `[W0C]` |
| 1 | flags: `nb << 1 \| static`, `nb` = BTOC VBAs | `[THIS: 1033 SBs]` |
| 2–7 | weave of the SB's first data VBA | `[W0C sb1 offset 4088]` |
| 8–11 | u32 `max_weave_add` | `[THIS]`, §4.3 |
| 12–13 | u16 `nb` | `[W0C]` |
| 14–15 | `0xffff` | `[W0C]` |

**Context metadata** (`type = 0x1f`):

| bytes | field | evidence |
|---|---|---|
| 0 | `0x1f` | `[W0C sb519]` |
| 1 | section (§4.4) | `[W0C sb519]` |
| 2–7 | weave, +1 per LBA | `[W0C sb519]` |
| 8–9 | u16 offset of this LBA's part within its section, in entries | `[W0C sb519]` |
| 10–11 | u16 length of this LBA's part, entries | `[W0C sb519]` |
| 12–15 | `0xffffffff` | `[W0C sb519]` |

### 4.2 Superblock lifecycle

A superblock is written front to back; the last VBAs of the superblock receive the BTOC.
An SB whose first VBA reads erased is treated as unwritten by a scan
`[THIS]`, `[DEV 2026-10-27: a frontier that left its first VBA clean was read as an erased SB]`.

### 4.3 BTOC

The block table of contents is at the **end** of a closed superblock, at the last
`nb` VBAs of its capacity (`width·1024`), with `nb = 4` or `8` (4 VBAs per
16 KiB page; 8 when entries + terminator need more than one page)
`[THIS: W0C, 487 + 156 + 390 SBs]`.

Entry: 16 bytes, four u32 LE:

| | field | evidence |
|---|---|---|
| 0 | `weave_offset`: weave of the run's first VBA minus the SB's first weave | `[THIS: 39 SBs rebuilt byte for byte]` |
| 4 | user sequence of the run's first VBA | same |
| 8 | LBA (or token) | same |
| 12 | count | same |

Rules, all confirmed by rebuilding BTOCs from page metadata alone
`[THIS: 39/39 SBs identical, §8]`:

- Entries appear in **VBA order**. The position (`VBA` offset) of a run is the
  sum of the counts of the entries before it; the entry itself carries only the
  weave offset, which differs from the position when there are weave gaps.
- A run is a sequence of consecutive VBAs with consecutive LBAs **and**
  consecutive weaves. A change of `user` does not end a run.
- Each token (`PAD`, `DELETE`) is its own entry with count 1.
- A terminator entry with `count = 0` ends the list; the rest of the BTOC is zero.
- `max_weave_add` = last data weave − first data weave + 1, or that plus `nb`; iOS
  writes both forms (21 and 18 of 39 sampled) `[THIS]`. A Linux close writes the
  `+ 0` form `[DEV 2026-10-27]`.

BTOC size: `nb = 8` for 1209+ entries, `4` otherwise (995, 1013 entries use 4)
`[W0C]`, `[DEV 2026-10-27]`. A closed SB holds `capacity − nb` data VBAs.

### 4.4 Context

One context copy occupies 14 pages of an all-4-bank superblock: 56 LBAs,
written at weaves +0..+55 from a base weave `W`, in this order `[W0C sb519]`:

| section | flag byte | layout | pages | evidence |
|---|---|---|---|---|
| base | 1 | see below | 1 | `[W0C sb519 p0 b0]` |
| stats | 2 | opaque, see below | 1 | `[W0C sb519 p0 b1]` |
| SB array | 3 | 8 bytes per SB, 512 per LBA | 1 (31 entries in the 3rd LBA, 4th LBA len 0) | `[W0C sb519 p0 b2]` |
| user sequences | 5 | 16 bytes per SB, 256 per LBA | 2 | `[W0C sb519 p0 b3 – p1 b0]` |
| read counts | 6 | u32 per SB, 1024 per LBA | 1 | `[W0C sb519 p1 b1]` |
| tree | 4 | §4.5 | 8 (32 LBAs) | `[W0C sb519 p1 b2 – p3 b1]` |

Each section starts on a page boundary. Sections shorter than a page are padded
with LBAs of length 0 (`offset` keeps counting in LBA units: SB array offsets 0, 512, 1024,
1536; user sequence offsets 0, 256, …). Bytes after a section's `len` in an LBA carry
no meaning — iOS leaves stale buffer contents there (plain text and PNG bytes
were seen) `[W0C sb519]`. Comparisons of generated contexts ignore them.

- **Base section** (one page): u32 `1`, u32 `519` (this context's own superblock
  number) `[W0C sb519]`, zeros, and u32 `1055` as the last word of the page's
  final LBA `[W0C]`. See §9 for the meaning of the first words.
- **Stats section** (one page): contents opaque; about 920 bytes in the first LBA,
  the rest zero, and u32 `1055` as the last word of the final LBA `[W0C sb519]`.
  A commit writes back the stats page **unchanged**, read from the context it
  replaces, and refuses to commit without one `[DEV 2026-10-27, 2026-10-27 W2 prep]`.
- **SB array**: per superblock `u8 type, u8 btoc_flags, u16 valid, u16 reserved(0),
  u16 erases`:
  - `valid` = number of LBAs the tree maps into this SB, exactly `[THIS: all 1054 SBs]`.
  - `btoc_flags` = the BTOC metadata flags byte of the SB; 0 if not closed
    `[THIS: 1033 closed SBs]`.
  - `erases`: erase count. `reserved` is 0 on all 1055 entries `[W0C]`.
  - `type` values seen `[THIS: W0C]`:

    | value | count | observed state |
    |---|---|---|
    | 1 | 5 | no valid data; awaiting erase (some already erased, some already rewritten after the context) |
    | 3 | 1033 | closed data SB with BTOC |
    | 4 | 3 | open data SB (no BTOC yet, partly written) at context time; iOS continues appending after the context (sb145 and sb599 did) |
    | 5 | 1 | sb764, `btoc_flags = 9`, valid 3888; rewritten after the context. Meaning not fully established, §9 |
    | 7 | 2 | context: sb519 is this context, sb232 was the previous one, already erased |
    | 8 | 1 | sb0, no blocks |
    | 9 | 10 | erased, ready for reuse |
- **User sequences**: per superblock four u32 `min, max, average, count`
  over the BTOC entries of a closed SB `[THIS: all 1033 closed SBs]`:
  `min`/`max` of the entries' user fields, `average = floor(sum/count)` over all
  entries (not weighted by run length), and `count` = number of BTOC entries.
  `[DEV 2026-10-29]` found earlier Linux contexts to be wrong on these summaries:
  they are computed at close, before the context is built.
- **Read counts**: u32 per superblock; opaque to the mapping but carried across commits.

### 4.5 Tree (the map on media)

Every tree LBA: first LBA as u32 (`0` for the first), `0xfffffff0` as a marker,
then **511 runs** of `(u32 vba, u32 span)`, then `0xffffffff` if the LBA is short
`[W0C sb519, THIS]`. Runs are ordered by LBA and cover the LBA space in order
from 0; a VBA of `0x7fffff` means unmapped. Each tree LBA is full except the last
used one; iOS's 13026-run tree used 26 LBAs of 32 (25 full + one with 251 runs).
Unused tree LBAs carry `0xffffffff 0xffffffff` in their first two words and are
written regardless; the tree is **32 LBAs always** `[W0C sb519, DEV 2026-10-27
(sb232, sb519), 2026-10-27: Linux commits with 25 LBAs + padding to 32 were
accepted by iOS]`.

The tree spans the whole LBA space including the hidden range (§4.6).
The loaded tree's runs may be split differently from a freshly built one;
two trees are compared by **normalising** adjacent runs that continue each other
`[DEV 2026-10-27]`.

### 4.6 LBA space, hidden LBAs and tokens

- Reported LBAs: 3906250 (16 000 000 000 bytes) `[W0C VFL header, LwVM media size]`.
- Mapped space (tree total): 3906266 `[THIS]`. The 16 LBAs 3906250..3906265 are
  hidden from the block interface:
  - 3906250..3906264 unmapped in iOS's context `[W0C, DEV 2026-10-27]`.
  - **3906265 is used by iOS** (internal data, type 2): at this dump VBA `0x9157b` in the
    context, moved to `0xa6513` by replay `[THIS]`.
  - A driver never writes the hidden LBAs. Valid data at hidden LBAs **must move
    with GC** as data of type 2 `[DEV 2026-10-27]`.
- Tokens: `0xffff0000` PAD, `0xffff0001` DELETE. iOS writes DELETE as a data VBA of
  type 2 whose 4 KiB payload is `u32 n`, then `n` pairs `(u32 lba, u32 count)`
  `[THIS: W0C sb164 offset 2703, first count 0x1fe]`. On replay the listed LBAs become
  unmapped. Linux need not emit DELETE (a filesystem mounted without discard
  does not need them) but must honour iOS's on replay `[DEV 2026-10-27]`.

### 4.7 Replay of unclean state

Mount sequence (all steps read-only):

1. Find the newest VFL context (§3.5), load the usage tables.
2. Find context SBs: scan the first VBA of every superblock and collect those of
   type `0x1f` with weave `W` (the "base weave" of a context copy); take the newest. Other
   copies are ignored. On this media there can be up to 2 candidates
   `[DEV 2026-11-07]`. Read 56 LBAs.
3. Load the tree (§4.5) as the map.
4. Replay set = **{SBs whose first-VBA weave > W}** ∪ **{SBs of type 4 or 5 in the
   loaded SB array}** `[THIS: W0C: fresh 764, 164, 166; open 119, 145, 599]`.
5. Process the replay set in ascending first-weave order:
   - **Closed** (last VBA has type `0x1c`) and first weave > W: read BTOC, walk
     entries with cumulative VBA position, map each LBA of each run to its
     VBA (DELETE: unmap listed ranges, PAD: nothing).
   - **Otherwise** (open, or a type-4/5 SB without BTOC, or closed type 4/5 not
     newer than W): walk VBAs in order until an erased VBA or a BTOC page is reached;
     **apply only LBAs with weave > W**; same token handling.
6. Result: the live map. On `[W0C]`: mapped LBAs 747503 → **743683** after replay, and
   **120 000 random mapped LBAs all read metadata matching their LBA** `[THIS, §8]`.

The SB array is not recomputed for a read-only mount; a writer must correct it
(§4.8). A mount that replayed anything is due a checkpoint at the next opportunity
`[DEV 2026-11-07]`.

iOS's own replay handles Linux SBs that are closed with BTOC `[DEV 2026-10-27]`; a Linux SB left open with
no BTOC is replayed by Linux `[DEV 2026-11-07]`; what iOS does with such an open SB is
untested, see §9.

### 4.8 Writing the FTL

Required behaviour (design is free as long as these hold). Each is a
device-proven property of a writer that iOS accepted.

**Frontier**:

- A frontier SB is erased first, then written front to back: data slots in order,
  a page at a time, four LBAs per page.
- Weaves are allocated strictly increasing for each VBA, starting above every
  weave on the media (i.e. above the newest context's base weave after replay
  has advanced past everything seen) `[DEV 2026-10-27]`.
- Partial pages are filled with PAD slots (§4.1); a partial page rewrite is
  read-modify-write of the 4-LBA page: read the live siblings, write them with their
  stored user sequence into the new page's slots `[DEV 2026-10-29]`.
- On close: write the BTOC (§4.3) in the last `nb` VBAs, then the tail is done.
  User-sequence summaries (min/max/avg/count) are computed from the BTOC at close
  and carried into the next context `[DEV 2026-10-29]`.
- Never issue a program whose failure would be silently discarded: on any program
  or verify failure, stop the writer (no more acknowledgements) `[DEV 2026-11-07]`.

**Commit (context checkpoint)**:

1. Close the frontier (BTOC). Without a closed frontier nothing committed is
   recoverable by BTOC; an open frontier is replayable only by metadata scan.
2. Build the context: the current SB array (with corrected `valid`, `btoc_flags`,
   `type`), user-sequence summaries, reads, the kept stats page, and the tree, per §4.4
   and §4.5 with the tree padded to 32 LBAs.
3. Write to a clean, erased spare SB with all four banks: 14 pages, base weave above all
   weaves on the media. Verify every page by reading it back
   `[DEV 2026-10-27: 14 pages verify clean]`.
4. Only after the new context verifies: older contexts retire (they may be erased); GC
   victims marked for erase are erased after the commit, never before. If a crash
   happens before step 3 finishes, the old context plus the replay still describes
   a consistent map `[DEV 2026-10-27]`.
5. Retain the previous context SB as a fallback, so at most **2 context SBs** exist at
   any time after a commit; iOS's own rotation already does this `[DEV 2026-11-07]`.

**Garbage collection**:

- Victims are closed, non-static (flags without `0x01`), non-open SBs. Everything
  still mapped into the victim moves, **including hidden LBAs**, keeping each moved
  page's type and original `user` `[DEV 2026-10-27]`.
- DELETE tokens need not be moved: the committed tree already holds the trims
  `[DEV 2026-10-27]`.
- The victim is erased only after the next context is committed.
- A GC's first real run used a victim with few valid LBAs; finding live pages via a walk
  of the map works and iOS accepts the result `[DEV 2026-10-27, 2026-11-08]`.

**Allocation of SBs**: use only SBs with type 9, or type 1 after erasing, with no
bad bank restrictions beyond §3.3. Keep a reserve: at least enough spares for one data
frontier plus one context plus the next context, and don't let the reserve run out
during a reclaim (`[DEV 2026-11-07: reclaim floor 6 spares, up to 4 victims per
reclaim]`).

**Static SBs** are never touched by the writer `[DEV 2026-10-27]`.

## 5. Coexistence with iOS (the hard requirement)

Everything observed about iOS meeting Linux's writes. Each is a device measurement.

- iOS boots on contexts written by Linux in the §4.4 format, loads them, and carries on
  **above** the Linux base weave. Twice on 2026-10-27: Linux context sb953 and
  sb166 accepted, iOS later committed its own context above them
  `[DEV 2026-10-27]`.
- iOS treats older contexts as its own: it **erases** them and reuses the SBs as data
  (sb953 → erased; sb350/sb166/sb347 → data) `[DEV 2026-10-27]`.
- A Linux data SB closed with BTOC stays as an ordinary data SB; iOS keeps the
  data (sb134: 7 LBAs read with the same hashes after iOS ran) and frees Linux's context
  SB `[DEV 2026-10-27]`.
- A real Linux GC (victim erased, data moved, commit) followed by iOS boot: iOS took
  its erased victim SB and refilled it; moved LBAs intact `[DEV 2026-10-27]`.
- The context moves between Linux boots; iOS runs between them and commits its own.
  Every mount starts from whatever is newest on the media `[DEV 2026-10-27]`.
- On a writable boot cycle, iOS, after Linux's checkpoint: erased Linux's older context SB,
  reused the SB, kept every byte of the Linux volume (`4e433cd8...` image hash) and all
  88 test files; other LBAs hash the same `[DEV 2026-11-07]`.
- Full cycle with a real root: Linux writes root on the volume, reboot,
  checkpoint, iOS, NAND root again: filesystem clean, journal not recovering
  `[DEV 2026-11-07, 2026-11-08, 2026-11-09]`.
- iOS rewrites its own superblocks freely (block 0 of CAU 0, `[DEV 2026-10-27]`)
  and modifies `Data` content between Linux boots; hash comparisons on `Data` must allow
  for this (LBA 2563470 and similar change on every iOS run) `[DEV 2026-11-07]`.
- Two historic failures, both on **media not in spec format**, that must not recur:
  a third LwVM volume past iOS's limit hung iOS at the logo (§6); a Linux
  install with bad PAD fill and zero user sequences preceded boot loops of
  unproven cause `[DEV 2026-10-29]`. The format rules of §4 exist because of them.
- An iOS fsck after a large Data shrink reports rc 8 until a clean restart; this
  is not a Linux fault `[DEV 2026-11-08]`.

## 6. LwVM

The header lives at LBA 0 (primary) and LBA 4095 (backup, last LBA of chunk 0);
both copies identical `[THIS: W0C VBA 0x35141a, 0x35141b]`. 4096 bytes:

| offset | field | evidence |
|---|---|---|
| 0x00 | magic GUID bytes `6a9088cf 8afd630a e351e248 87e0b98b` | `[W0C LBA 0]`, `[KC string "LwVM: media header mismatch" 0xa5fc82]` |
| 0x10 | media GUID | `[W0C]` |
| 0x20 | u64 media size, `16000000000` | `[W0C]` |
| 0x28 | u32 number of partitions | `[W0C]` |
| 0x2c | u32 CRC-32 (zlib) of the whole 4096 bytes with this field zeroed | `[THIS]` |
| 0x30.. | zero | `[W0C]` |
| 0x200 | partition entries, 128 bytes each (below) | `[W0C]` |
| 0x800 | chunk map: 1024 u16 | `[W0C]` |

Partition entry (128 bytes):

| offset | field |
|---|---|
| 0 | type GUID, stored as raw bytes (HFS+: `48465300-0000-11AA-AA11-00306543ECAC`) |
| 16 | unique GUID |
| 32 | u64 start byte offset, in LwVM's virtual space |
| 40 | u64 end byte offset, exclusive |
| 48 | u64 attributes (e.g. Data = `0x3000000000000`, effaceable) |
| 56 | UTF-16 name (e.g. `System`, `Data`, `Linux`) |

`[W0C, I1 header of 2026-11-07 (3 volumes)]`.

Chunk map entries, one per physical chunk of 16 MiB = 4096 LBAs:

| value | meaning |
|---|---|
| `0xF000` | header (chunk 0) |
| `p<<12 \| i` | logical chunk `i` of partition `p` |
| `0xF111` | free chunk on the media |
| `0xF3FF` | number past the media's end (never on media) |

`[W0C]`: 1024 entries, 133 System, 813 Data, 1 header, six free (947–952,
`0xF111`), the rest past the end.

**The map is permuted**: physical chunk numbers do not follow partition chunk indices
(Data chunks 807–812 are at physical 138–143 in one layout). Never infer positions by
counting `[DEV 2026-10-29]`.

**Capacity rule.** iOS's validator reserves 8 full chunks from the virtual
address space, so a partition end must be at most
`0x4000 + 2·4096 + (chunks − 8)·16 MiB` = **15854493696** for this media (953 full
chunks). Physical free chunks past that bound do not help. A third volume whose
end exceeded the bound **hung iOS at the Apple logo**
`[DEV 2026-10-29]`. iOS's real validator, run under Unicorn on this kernelcache:
- header of 2026-11-07, three volumes (Data shrunk, Linux 15753830400..15854493696): CRC 0,
  validator 0 `[THIS]`
- the same with Linux's end +96 MiB: CRC 0, validator −1,
  `"LwVM: partition %d ends too high"` for partition 2 `[THIS: strings at file offset 0xa5fd44;
  entry points KC VA 0x80a9eb60 (property init), 0x80a9eebc (CRC), 0x80a9ef80 (validate)]`.

Other validator strings at file offsets `0xa5fc82`–`0xa60166` list the accepted
shape (partition count, order, alignment, chunk ownership) `[KC]`.

A new layout must be checked against this validator, offline, with the exact header
before any write; the third volume was made with a native HFS shrink of Data first, the
header only after `[DEV 2026-10-29, 2026-11-08]`. Header writes go backup first (LBA 4095),
then primary (LBA 0), through the FTL, verify each `[DEV 2026-10-29]`. Header changes
are never made by the running writer; this is a separate deliberate operation.

Today's layout (2026-11-08): System 133 chunks, Data 301 chunks with an HFS of
1230568 blocks, Linux 512 chunks (8 GiB), CRC `d32fc8ee`, Data ending at 7264559104,
Linux ending at 15854493696 `[DEV 2026-11-08]`.

The Linux-side LwVM reader validates CRC and shape against the rules above and exposes
one block device per partition (`ftl0p0`, `ftl0p1`, `ftl0p2`) `[DEV 2026-10-29]`.

## 7. Required Linux-side behaviour

### 7.1 Write scope

Writable data is exactly **one** LwVM volume, named `Linux`. The whole-LBA
device, `System`, `Data`, and any partition with the effaceable attribute are
read-only: requests to write them fail at the block interface (not only by a
read-only disk flag) `[DEV 2026-10-27]`.

### 7.2 Durability

- No volatile write cache: a write bio is complete when its pages are programmed
  (and verified per policy, below). Flush/FUA need no extra work and the queue
  advertises no cache. Do not advertise NOWAIT; operations sleep `[DEV 2026-11-07]`.
- Writes and any operation touching media serialize behind one per-instance lock.
  Detach marks the instance going away, removes disks without holding it, then frees
  `[DEV 2026-11-07]`.
- Checkpoint (close frontier + commit) at unmount and at reboot/power-off for a
  writable mount, even without writes if the mount replayed
  `[DEV 2026-11-07]`. A failed close "wedges" the writer: every later write and
  checkpoint fails until remount; replay recovers what was programmed
  `[DEV 2026-11-07]`.
- Verify policy: read-back of every Nth programmed page, N = 16 by default; the ladder
  (§8) runs with verify on every page first `[DEV 2026-11-08]`.
- Frontier open across flushes: one superblock is not closed per journal commit
  (it would use a 16 MiB SB each time) `[DEV 2026-11-07]`.

### 7.3 Reclaim and checkpoints

When spares fall below the floor (6), at the top of a write: up to 4 victims
(closed, non-static, or the frontier rotated first if residents would not fit), then a
checkpoint, which erases the victims `[DEV 2026-11-07]`. Bounded contexts: 2 SBs at
most; a retired context is erased only after the new one verifies `[DEV 2026-11-07]`.

### 7.4 Write-gate interface used by stage 1

Exact interface of `devices/a5/initramfs/sbin/p105-nand-root`, which must keep working
(the script reads these paths):

| parameter | behaviour |
|---|---|
| `/sys/module/apple_ppn/parameters/hold_probe` | writable; `1` on the command line defers the hardware probe until stage 1 writes `0` (the PPN probe sleeps, so it needs a working tick); `0` releases it |
| driver binding | the platform device `31200000.ppn`, bound through `/sys/bus/platform/drivers/apple-ppn/bind` |
| `/sys/module/apple_ftl/parameters/mount` | write `1` to mount the FTL and create `/dev/ftl0p*`; must be writable (mode `0200` or similar) |
| `/sys/module/apple_ftl/parameters/enable_write` | read-only flag set on the command line; `Y` = the FTL may write |
| `/sys/module/apple_ftl/parameters/write_volume` | read-only, name of the only volume that may be writable; stage 1 requires it to equal `Linux` when `enable_write = Y` |
| `/sys/module/apple_ppn/parameters/allow_write` | read-only; `Y` required when `enable_write = Y`, `N` when not; absent is acceptable only when `enable_write ≠ Y`. Without it set, program and erase return an error |

Stage 1 then expects `/dev/ftl0p2` to exist, `/sys/class/block/ftl0p2/ro` to be `0`
when writable, and mounts ext4 with `rw,noatime,commit=30`.

Kernel command line of the nand-rw profile: `root=/dev/ftl0p2 rw apple_ppn.hold_probe=1
apple_ppn.allow_write=1 apple_ftl.auto_mount=0 apple_ftl.enable_write=1
apple_ftl.write_volume=Linux` `[DEV 2026-11-07, script]`.

Volumes refused even when named: `System`, `Data` (any case), effaceable ones
`[DEV 2026-10-27]`.

### 7.5 Performance targets

| | target | measured with the existing writer |
|---|---|---|
| sequential write, ext4, large | 8–10 MiB/s | 8.0–10.2 MiB/s on root, 9.2 without verify `[DEV 2026-11-08/09]` |
| sequential read | 8 MiB/s | 8.0 MiB/s `[DEV 2026-11-08]` |
| synced 4 KiB (fsync) | ≤ 40 ms | 27 ms `[DEV 2026-11-09]` |
| page program | | 1.23 ms total, 0.73 ms transfer `[DEV 2026-11-08]` |

Method: writes gather whole bios (avoid one program per 4 KiB bvec), program up
to 32 pages in one batch, one page per CE at a time with overlap between the
two CEs `[DEV 2026-11-08]`.

## 8. Acceptance tests

All of these need no old code.

### 8.1 Offline against the dump `w0c`

Reader written from this spec, run over the 17 GB dump. Expected values (each
measured in the writing of this spec, `[THIS]`):

| check | expected |
|---|---|
| VFL context live on block 1063, 59 log pages per bank | yes |
| Bank tables: usable block counts | banks 1053 / 1053 / 1051 / 1054 |
| Context SB | 519, base weave `0x162d1045` = 372052037, 56 LBAs, sections 1,2,3,5,6,4 |
| Tree | 13026 runs, covers exactly `0..3906265`, 26 used LBAs of 32, last used LBA 251 runs |
| `valid` in SB array | matches tree counts on all 1054 SBs |
| Closed SBs | 1033, all with BTOC of `nb` 4 or 8; flags `8:487, 16:390, 9:156` |
| BTOC rebuild from page metadata | byte-identical on the 39 SBs tested (random sample + sb2, sb4, sb5) including gaps |
| User sequence summaries | `min/max/average/count` from BTOC entries match on all 1033 |
| Tokens in BTOCs | 339 DELETE, 32 PAD |
| Replay | fresh: sb764, sb164 (closed), sb166 (open, 1300 VBAs); open at context: sb119 (no newer writes), sb145 (2684 newer), sb599 (3960 newer); mapped 747503 → 743683 |
| Whole-map check | 120 000 random mapped LBAs: metadata LBA equals the mapped LBA, 0 mismatches |
| Hidden LBA 3906265 | mapped at VBA `0x9157b` in the tree, `0xa6513` after replay |
| LwVM at LBA 0 | System 24576..2224152576, Data ..15854493696, CRC valid, primary = backup |

**Golden context**: serialise a context from a context loaded from the dump and
compare byte for byte with iOS's sb519 (metadata in full, data as far as it is content:
stop at `len` for SB/user/read LBAs, at the end of runs for tree LBAs). Repeat for any later iOS
context obtained.

**Golden BTOC**: rebuild the BTOC of closed SBs from their data VBAs' metadata;
identical to the dump, as above, on all 1033 for a full acceptance.

**LwVM**: the iOS validator (under an emulator, offline, on the 12H321 kernelcache) accepts
any header the driver would ever write; a header exceeding the capacity rule is refused
with the message of §6.

### 8.2 Device ladder (writes are not done by this session)

Each step stops on any unexpected status, failed verify or general error; the bank of
tests is reset before proceeding. Ordered, one kind of risk at a time:

1. **Read-only**: mount; replay; `System` file set against the IPSW rootfs (56877/56881
   identical in the earlier run; the four others are the jailbreak); samples equal to
   the iOS view; no write code linked. Run a long read pass right after boot.
2. **Reserved blocks**: program/erase/read cycles (PIO, MLC and SLC, all pages) on the
   six blocks marked `0xbff0` available (bank 0 block 1054, bank 1 blocks 1054–1056, bank 3
   blocks 1055–1056), leaving them erased; neighbours unchanged; iOS boots after.
3. **FTL writes that are undone**: data to a spare SB + BTOC + erase; compare map digest before and after;
   a BTOC parse equals what was written.
4. **Context write, unused**: a whole context on a spare SB, verify each page, erase it.
5. **Linux context**: commit on a spare SB, remount and load it; withdraw it again
   (erase the Linux context, remount: loads the iOS copy). Only in the Linux session.
6. **iOS boot on a Linux context**: then back to Linux: iOS's new context loads; golden
   the new context; check what iOS did with the old ones. Do this twice.
7. **Real GC** + commit + iOS boot. Moved LBAs hash the same.
8. **The volume**: `write_volume=Linux`; ext4 tests, fsync/rename load, crash (no
   checkpoint) replay, checkpoint rotation, reclaim, unmount checkpoint; iOS after each.
9. **Root on NAND**: reboot cycles, Linux↔iOS, boot counts, hashes.
10. **Sustained**: speed vs §7.5; long read passes; iOS after every stage.

## 9. Unverified

Facts here are known only from earlier driver behaviour or are inferred, and could
not be confirmed from a binary, a dump or a measurement while writing this spec.

- §2.9, all `[EARLIER DRIVER]` steps. Confirmed when a read-only build following it reads
  many consecutive pages on one CE with data equal to the `w0c` reference (allowing for what
  iOS changed since), across both CEs and CAUs, and the reads survive repeated module loads.
  Cross-checked against iBEC so far: command opcodes, event-clear values, poll set-up
  (iBEC differs: `F+0x10 = 0x100` and an interrupt), `0xffff` after reset, a write to
  `F+0x80014` at init. Not cross-checked: order of the bring-up steps, the 16-CE reset loop,
  the 10 µs and 50 ms delays, bit 8 of `F+0x0c` as the poll's end condition.
- The items tagged `[EARLIER]` in §1.3 and §2.5: the order of the fields of the 8-byte
  read address, the 4 × 1028 transfer shape, software reset around every operation,
  and which of these is what lets the next operation on a CE work (not established:
  they were never varied separately on the device).
- Exact `FMI_CONFIG` for whole-page **PIO transmit**. Receive uses 5 (`[iBEC]`,
  dump); the earlier driver's transmit worked on the device (W1 42/42) but the precise
  value is established only by that driver. Implement starting from 5, confirm in
  ladder step 2 (reserved blocks), first program.
- Polarity and encoding of the bad-block bitmap (feature `0x84 | cau<<8`); the driver
  does not need it (use the VFL tables).
- The meaning of base-section words 0 and 1 (`1`, own SB number) beyond their observed
  values on the one iOS context in `[W0C]`.
- SB-array `type` values 1 versus 9 and 5: observed states are listed in §4.4; which value a
  writer should use for a retired context is inferred (iOS re-listed retired Linux
  contexts as erase-before-use); use value 1 for a retired or emptied-but-unerased SB,
  9 for a known-erased SB. Only type 7 (context) and 3 (closed data) are certain.
- Whether `0x87` without confirm (multi-page) can work on this package with a
  sequence not yet tried (`[DEV 2026-11-09]`); unused here.
- Read clock sweeps beyond the data capture delay (`rs`, `rh`, faster read clock):
  never done; read speed is 8 MiB/s.
- What iOS does with a Linux frontier left **open** (no BTOC) by a crash: never tested;
  iOS boots on closed-SB contexts only. Avoid by checkpointing; report if a crash left one.
- Power loss during a multi-page program batch or during a context write: never
  measured on the device (only the unload-without-checkpoint crash test passed).
- Whether DELETE (trim) tokens written by Linux would be accepted by iOS: untested;
  Linux does not emit them.
- The reason for the boot loop after the historic W4 install (§5); the format
  corrections made afterwards stopped it but the cause is unproven `[DEV 2026-10-29]`.

## 10. Open questions for the user

1. Is reading the iOS-side `/dev/rdisk0` view for samples acceptable as part of the
   ladder after each iOS boot (iOS SSH), as in earlier runs?
2. Should the rewrite keep the sysfs paths `apple_ppn` / `apple_ftl` exactly (as
   §7.4 assumes) or may stage 1 change with it?
3. Are further raw dumps (a fresh `w0c`-style dump after iOS has run on a Linux context,
   or a full dump of today's state) wanted to complete the golden tests beyond the one iOS
   context available here?
4. The earlier driver's behaviour on program failure was to stop writing; is "no retire,
   end of session on program error" acceptable as the rewrite's first policy, or is a
   bad-block handling path required before use?
5. Any requirement for the rewrite to emit DELETE tokens (trim / `fstrim`), or remain
   without (recommended, as tokens are untested from Linux)?
