# P105 NAND rewrite: gaps and corrections to the spec

Companion to `p105-nand-spec.md`, kept by the clean-room implementation in
`devices/a5/patches/files/drivers/mtd/nand/apple/`. Everything here is either a
place where the spec and the dump disagree, or a decision the spec left open.
The hardware findings behind the spec, and the write path that first proved
this package writable, are the work of **crystal-lized**.

Source of each item: the `w0c` dump (`[DUMP]`) or the iBEC (`[iBEC]`). No other
source was consulted.

## Corrections: where the dump disagrees with the spec

| spec | what the dump shows | what the code does |
|---|---|---|
| §3.5 signature word `0x77726d78` ('xrmw') | The bytes are `78 72 6d 77` = `0x776d7278` LE; `0x77726d78` would read "xmrw" `[DUMP bank 2 block 1061]` | Compares against `0x776d7278` |
| §4.3 "each token (PAD, DELETE) is its own entry with count 1" | True for DELETE. All 32 PAD entries have count 16, and all 32 are in one superblock, sb243 `[DUMP]`: there a run of PAD slots with consecutive weaves is one entry, split every 16 VBAs. The cap of 16 rests on this one superblock | `sftl_btoc_append` merges PAD runs up to 16 |
| §4.3 "`nb = 8` for 1209+ entries, 4 otherwise" | The choice is not a function of the entry count: nb 4 is seen with up to 1022 entries, nb 8 with as few as 1014 `[DUMP]` | Linux uses 4 when entries + terminator fit a page, 8 otherwise, and reads either |
| §8.1 golden BTOC "identical on all 1033" | 1026 of 1033 are identical. The other 7 (sb 158, 299, 349, 486, 610, 612, 745) differ only in the `user` field of 14 entries, where iOS's BTOC value differs from the page's own metadata. Every other field matches `[DUMP]` | Not reproducible from page metadata alone. Linux writes the page's `user`. Whether iOS reads this field back is unknown |
| §4.3 "sum of counts" / closed SB holds capacity − nb data VBAs | Confirmed on all 1033: 0 exceptions, so iOS never leaves a hole before a BTOC | See decision D1 |
| §2.1 page size field = 16384 | The field at 0x34 reads 0x4040 = 16448 on this package, so it counts the 64 metadata bytes `[DEV 2026-11-09, rung 1]` | The driver checks for 16448 |
| §2.4 row address "three bytes" | On this package (PPN 1.5.5) the row is sent as **4 bytes**: iBEC sets the address width to 4 for PPN versions above 1.4.ff, `[iBEC 0x9ff084c0, 0x9ff08502]`, and uses it for read, program and erase. With 3 bytes every read ends in status `0x51` `[DEV 2026-11-09, rung 1]` | The driver sends 4 bytes (top byte 0) |
| §1.3 bring-up has no power-state step (mentioned only in §2.7) | After a NAND reset iBEC sets feature `0x180` to 2 (async; 0xA for DDR) `[iBEC 0x9ff095e4, 0x9ff08c0a, 0x9ff09668]`. Without it every array read ends in `0x51` `[DEV rung 1]` | The driver sets it after every reset |
| §9 transmit CONFIG unverified | `CONFIG = 5`, `CONTROL = 5` per burst, then wait for `F+0x0c` bit 1 (write-1-clear) after each burst `[iBEC 0x9ff0770e, 0x9ff07730]` | Done in `tx_burst`; first exercised by the power-state set |
| §8.1 tree "13026 runs" compare | iOS's tree carries splits at adjacent continuing runs. Normalised, both sides are 12938 runs and identical | Golden compare is after normalising, as §4.5 allows. Linux writes the normal form |

## Blocking rung 1 (2026-11-09): the second page read on a CE

Status: **open, needs a fact from the spec side.** Everything below was seen on the
device with the read-only build (no program or erase code); the only commands
beyond reads were NAND reset and set-feature `0x180`.

What works: bring-up, ID, parameters, firmware feature, power state, and the
**first** page read after a bus reset on each CE. Its data and metadata are
right (`20 ff ff 00 ...` on VFL page 0; 16448 bytes, status `0x40`).

What fails: the next operation on that CE, on any CAU and any address. The status
poll never matches, `C+0x48` reads `0x00`, and it times out; later operations
answer `0x51`. Only a full reset (block reset, NAND reset, power state) brings the
CE back, for one more read. With that retry per read the mount reached replay,
which took minutes and then failed.

Ruled out by experiment: which status form is used (`0x7d77` is the one iBEC uses);
skipping `0x77` after data; `0x77` before or after `CONTROL = 6`; waiting for and
clearing `F+0x0c` bit 1; 5 ms and 200 us delays; rewriting `CONTROL`, `ON` and
`IF_CTRL` after the read; clearing `DATANUM`, `PIO`, `CONFIG` first; repeating the
power state before each read. No more data follows the 16448 bytes (an extra
16-byte burst returns zeros). If fewer than all bursts are read, the next status
is `0x40` and the next read returns misplaced data: the poll dies only after the
whole page has been read out.

Question for the spec side: **what does a working page read do between the end of
data-out and the next operation on the CE**, and is the PIO burst pattern
(`1024 + 16 + 3072` per unit, each burst with its own `CONTROL = 3`) the one
that was used? iBEC's only direct page read is the boot-page path, which uses
`CONFIG = 0xF5`; its PPN pages go through the sequencer.

### Update, 2026-11-09, after spec commit b65857c

Each §2.5 item was tried alone and together on the first and following reads (read-only
build, bus 1 and bus 0): 8-byte full address; 4 x 1028 transfer; software reset before and
after the read. **None of them, nor all three, fixes the second read.** The 4 x 1028 shape
and the 8-byte address do return correct data on the first read. The failure is unchanged:
status poll times out with `C+0x48 = 0x00`, later operations answer `0x51`.

New observation: a status `0x77 0x7D` issued while no operation is pending, read straight
through the FIFO, returns `0x00`, and afterwards the package answers `0x11` / `0x51` to
everything until a full reset. Read ID still works in that state; the firmware feature
then fails. A status byte of 0 has bit 6 clear, which reads as "busy".

### Update 2, 2026-11-09, after spec commit 7c69941

Tried: `IF_CTRL = 0xffff` from the NAND reset until the power state is set, then the
working value; software reset around every operation, including ID, parameters and
features; the event clear and poll set-up of the spec (they already matched); a
2 s status timeout; a PMGR pulse after a read.

Results, none of which is a fix:
- Plain sequence of page reads on one CE: first read correct, second read hangs in
  status as before. The PMGR pulse after a read could not be judged: its only runs
  ended at the firmware check, before any page read.
- Setting the power state before ID and parameters breaks the firmware read (zeros).
  ID, parameters and firmware before the power state, at the slow timing, pass.
- If several feature reads come first (firmware x3, parameters, firmware), the hang goes
  away but the data is off by one operation: firmware #0 is right, #1 and #2 are zeros,
  the parameter read returns the firmware bytes, and each later page read returns the
  page requested **before** it (p1 returns p0, p2 returns p1, across CAUs too). Status
  is `0x40` throughout. A delay of up to 500 us before the status does not change this.
- A second status poll straight after a first one that returned `0x40` times out.

### Rung 1, 2026-11-09: context on the media with a tree longer than 32 LBAs

On the media now (context sb 555, written by the earlier Linux driver), the tree does not
stop at 32 LBAs: tree metadata (section 4, consecutive weaves) continues in pages 14 and on,
and the first word of each LBA continues the LBA count. Spec 4.5 says 32 LBAs always, which
holds for the iOS context in w0c only. The reader follows the metadata. **The writer still
keeps a tree within 32 LBAs** (D3).

## Decisions the spec left open

**D1. Closing a partly written superblock.** The spec says "write the BTOC in the
last nb VBAs" and does not say what happens to the VBAs between the last data
and the BTOC. iOS leaves no hole `[DUMP]`, and pages of a block go in ascending
order. The code fills the rest of the data area with PAD slots at close, so a
Linux SB has the same shape as an iOS one. PAD slots take a page program each:
at most about 1.3 s for a whole SB, at checkpoint only. The alternative, leaving
the pages erased with a shorter BTOC, is unproven against iOS. **Untested on the
device. Needs ladder rung 3.**

**D2. "Status poll" after read/parameters.** §2.5 and §2.1 read "status poll"; §1.5
defines both a bare poll and an operation status (`0x7d77` + poll). `[iBEC 0x9ff0758a]`
uses operation status for the matching get-feature sequence. The driver uses
operation status for get-feature, parameters and read. Unconfirmed on the device
for parameters and read until rung 1.

**D3. Tree overflow.** The spec fixes the tree at 32 LBAs (16352 runs) and does not
say what a writer does when fragmentation exceeds it. iOS's tree used 13026 runs
at the dump, so there are only about 3000 runs to spare. The writer compacts
before a commit: when over the limit it moves the most fragmented 1024-LBA
windows contiguously to the frontier, keeping type and user (like GC). Tested
offline with 2600 scattered writes (17615 runs, compacted, committed, remounted).
Not in the spec. To be judged at the ladder.

**D4. Replay order.** Replay walks superblocks in ascending first-weave order as
§4.7 says, and also keeps a per-LBA weave high-water mark, so the newest write
of an LBA wins even when its SB is walked earlier. Result on `w0c` is the
expected 743683 mapped, hidden LBA at `0xa6513`.

**D5. Binding.** The spec gives the device name `31200000.ppn` and driver name
`apple-ppn` but no DT node. The driver matches `apple,s5l8942-ppn` and takes its
register windows from fixed physical addresses (§1.1), not from `reg`. **The node
is not in this worktree's DT and is not added here.** Before ladder rung 1 the DT
in use must have a `ppn` node with this compatible, or the match table must gain
the running kernel's one.

**D6. In-tree build.** `Kbuild` and `Kconfig` are in the driver directory, for both
an external `M=` build and an in-tree one. Hooking them into
`drivers/mtd/nand/Makefile` and `Kconfig` is left to whichever step installs the
driver: the module build here does not touch the kernel tree.

## Checked only partially

- Partition address translation (chunk map, relative LBA 0 → FTL LBA): the HFS
  signature is checked on `System` only, because `Data` shows no HFS signature at
  that offset (probably encrypted: it carries the effaceable attribute; not
  confirmed), so its translation has no independent check.
- Three-volume header parsing: the I1 header from `nand-backup`.

## Open

- **Bad-block handling** on a program error: the first version stops writing for
  the session (user decision). To be added before any publication.
- All facts of spec §9 remain open: transmit `CONFIG`, the meaning of base words,
  the SB type values for retired SBs, overlapping programs on one CE, read-clock
  sweeps, iOS's behaviour on an open Linux frontier, power loss during a batch,
  iOS's reaction to Linux DELETE tokens, the historic boot loop. None was settled
  by this work.
