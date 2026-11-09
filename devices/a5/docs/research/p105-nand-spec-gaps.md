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
| §8.1 tree "13026 runs" compare | iOS's tree carries splits at adjacent continuing runs. Normalised, both sides are 12938 runs and identical | Golden compare is after normalising, as §4.5 allows. Linux writes the normal form |

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
