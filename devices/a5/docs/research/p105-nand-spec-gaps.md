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

### Rung 1 passed (user decision, 2026-11-14)

Read-only build on the iPad (commits 710b586, 6a67fe8): mount and replay in 6 s with
no read errors; 57022 of 57032 System files identical to the IPSW, the rest
jailbreak and housekeeping files; Linux ext4 volume mounts read-only; a read of the
whole 16 GB device in 565 s with no errors. Left open: the volume hashes differ
from those taken through the earlier driver before an iOS run (the System volume
header shows a modification after them), and no sample compare against the iOS
`/dev/rdisk0` view was made.

### Rung 2, 2026-11-14: reserved blocks, program and erase work

Test module `devices/a5/tools/nand-rung2` (commit c62a31e and the next), driver with
`allow_write=1`, on the rung-1 image (NFS root, no FTL loaded).

- **Page transmit.** It works with the shape of the receive (§2.5.1) mirrored: per
  4112-byte unit, clear events, `CONFIG = 5`, `F+0x34 = (1028 << 8) | 4`, `CONTROL = 5`,
  four times wait for `F+0x1c` bits 3–4 and push 257 words, then wait for `F+0x0c`
  bit 1 and clear it. No `CONTROL = 6` between the last unit and `0x17`. It was
  the first shape tried, and it read back right at the first program. This settles
  the §9 item on the transmit `CONFIG` (5). The burst form (`apple_ppn.tx_shape=1`)
  was not tried.
- **Program and erase.** One page MLC on bank 0 and one page SLC on bank 1, then a
  whole cycle on each of the six blocks (bank 0 block 1054, bank 1 blocks
  1054–1056, bank 3 blocks 1055–1056) in MLC (256 pages) and in SLC (128 pages):
  erase, program every page in ascending order with a distinct pattern in data and
  metadata, read every page back (status `0x40`, data and all 64 metadata bytes
  identical), erase, and every page reads `0x49` and all-FF in both modes. 12 of 12
  cycles, no error status. Erase used the MLC row (bit 23 clear) also after an SLC
  cycle.
- **Neighbours.** CRC over status, data and metadata of every page of the blocks
  next to each reserved block, read before and after: identical (bank 0 blocks
  1053 and 1055, bank 1 blocks 1053 and 1057, bank 3 blocks 1054 and 1057).
- **Timing**, one page per call, one bus, pattern generation included: 2.3–2.4 ms per
  MLC page, 1.8 ms per SLC page.
- Not exercised: programs overlapped across the two buses (the FTL's batch path);
  that comes with rung 3.
- Busybox `insmod` repeats a failed `finit_module` through `init_module`, so a test
  module that fails its init runs twice. The test module therefore always loads and
  reports its result in a parameter.

**Rung 2 passed (2026-11-14):** iOS booted after it (user check).

### Rung 3, 2026-11-14: FTL writes that are undone, passed

`devices/a5/tools/nand-rung3` (commit 62673ec): the same `rung3.c` passes on the `w0c`
overlay (`nandtest --rung3`) and on the device (`ppn_rung3.ko`, FTL core built in,
`apple_ppn allow_write=1`, rung-1 image, `apple_ftl` not loaded). On the device, in 27 s:

1. Mount: context sb 456, weave 377570546, 1187028 LBAs mapped, 13247 replayed, map
   digest `70b046e8c90958b1`.
2. Target sb 1048, four banks, listed erased, every page of its four blocks read
   erased; the writer fenced to it (commit and reclaim refuse, no other SB opens).
3. LBAs 123589..125588 rewritten with their own content: 1998 written, one sibling
   carried at each end; PAD fill to capacity, BTOC nb 4, 133 entries; every page
   verified after program (verify every 1). This is the first use of the batch
   program path, with pages overlapped across the two buses.
4. The BTOC read back through the mount path equals the one written and the one
   rebuilt from the metadata of all 4092 data VBAs; all 2000 LBAs read back right.
5. A fresh mount replays sb 1048 (15246 replayed) and its map equals the written one.
6. sb 1048 erased, every page reads erased.
7. A fresh mount: context, mapped count, replay count and digest equal step 1.

Nothing else on the media changed except the erase counts of the four blocks of sb 1048
(two erases each). No context was written.

**Seen on the way:** the live context is now sb 456, which iOS wrote after rung 2, and
its tree is **172 LBAs** long. iOS itself therefore writes trees longer than 32 LBAs when
the map is that fragmented (it kept the shape of the earlier Linux context, sb 555,
174 LBAs). The 32-LBA figure of spec §4.5 is the size of the iOS tree in `w0c`, not a
format limit. Decision D3 (compact to stay within 32) is therefore a choice, not a need.

### Context layout with a long tree, read from iOS's context sb 456 (2026-11-14)

Read only, with `devices/a5/tools/nand-rung4/ppn_ctxdump.ko` `[DEV]`:

- The base section holds `1`, the context's own SB (`0x1c8`) and `0x41f` at the end, and
  no other non-zero word: **no tree length is stored**.
- Tree LBA `t` is at VBA `24 + t`, for `t` past 31 too: the section continues in the
  pages after the fixed 14, metadata type `0x1f`, section 4, weave = base + VBA, the
  other metadata fields 0 / 0 / `0xffffffff` as in the fixed part.
- 172 tree LBAs (VBAs 24..195, the last with 154 runs), then 4 empty tree LBAs
  (`ffffffff ffffffff`, VBAs 196..199, same metadata), then erased pages from VBA 200.
  The context is 200 LBAs, 50 pages.
- Tree LBAs in the context: 176 for 172 used here, 32 for 26 used in `w0c`. The writer
  uses `max(32, roundup(used, 16))` (first `used + 1`, corrected at rung 9).
- The first weave of data written after it is base + 420 here, so the weave counter
  must clear the whole context (200), not 56: the loader now counts the context to its
  last section-4 LBA, empty ones included.

### Rung 4, 2026-11-14: a context written and not used, passed

`devices/a5/tools/nand-rung4` (commit 5b7a4d7), the same `rung4.c` as offline. On the
device, 19 s:

1. Mount: context sb 456, 200 LBAs (tree 172), digest `70b046e8c90958b1`.
2. Target sb 1048 (listed erased, every page read erased), writer fenced to it.
3. Erased, marked context, `sftl_context_write()` (the code commit uses): weave
   377583994, **200 LBAs in 50 pages, the same shape as iOS's**; every page read back
   equal to what was programmed.
4. A fresh mount loaded it: context sb 1048, 200 LBAs, tree 172, 0 replayed, the same
   map digest, and SB array, user sequences, read counts and statistics equal to what
   was written.
5. sb 1048 erased, every page reads erased.
6. A fresh mount is back on sb 456 with the step-1 digest and replay count.

The new context existed on the media for about 8 s (steps 3–5). No commit: no context
was retired, nothing else erased.

### Rung 5, 2026-11-14: Linux context committed, loaded, withdrawn, passed

`devices/a5/tools/nand-rung5` (commit b52af49). Before it, read only: one context on
the media, sb 1020, which iOS wrote in the boot after rung 4 (the SB array also lists
sb 456 as a context; the scan finds no context on it). On the device, 16 s:

1. Mount: context sb 1020, 200 LBAs (tree 172), 1188950 mapped, 14391 replayed, digest
   `b9d6876bf398e101`.
2. Target sb 1048, every page read erased; writer fenced to it with commit allowed.
3. `sftl_checkpoint()` → commit: context on sb 1048, weave 377602250, 200 LBAs in 50
   pages. Through a watcher refusing any other block: 4 erases, 50 page programs,
   0 refused. No context was retired or erased (only one was on the media).
4. A fresh mount loads sb 1048: same map digest, 0 replayed.
5. Withdrawn: sb 1048 erased, every page reads erased.
6. A fresh mount is back on sb 1020, same digest, 14391 replayed.

Not exercised here: retiring and erasing an older context at commit (none on the media),
and iOS booting on a Linux context (rung 6).

### Full dump before rung 6, 2026-11-14 (`r6pre-20261114`)

Taken with `devices/a5/tools/nand-dump` (read only, `apple_ppn` without `allow_write`)
after rung 5, on the rung-1 image, into `/Users/k/cascadia-root/srv/nand-dumps/r6pre-20261114`
(the NFS root as the iPad sees it: `/srv/nand-dumps/...`), in the `w0c` layout; 37 min.
SHA-256 of the 24 files: `devices/a5/output/dump/r6pre-20261114.sha256` (not in git).

- 1089536 pages, no failed read; statuses `0x40` 954375, `0x49` 130931, `0x61` 2182
  (SLC pages read with an MLC row), `0x45` 2048 (retired, as in `w0c`).
- The offline harness mounts it with the device's own result: context sb 1020, 200 LBAs,
  tree 172, 1188950 mapped, 14391 replayed, map digest `b9d6876bf398e101`. `--rung4` on
  its overlay passes.

### Rung 6, round 1: Linux context committed and left, 2026-11-15 — iOS booted on it

`devices/a5/tools/nand-rung6` (commits 0f82b18, febbdde). Read-only check first: clean.
The golden rebuild of iOS's context sb 1020 from its own tables is identical to the media
(200 LBAs: metadata of every LBA, base, statistics, SB array, user sequences, read counts,
tree 87503 normalised runs), so the long-tree layout above is what iOS writes, not only what
it reads. 20000 random mapped LBAs read with matching metadata; LwVM with System, Data, Linux.

Then `action=commit`: Linux context on **sb 1048**, weave 377602250, 200 LBAs (tree 172,
87684 runs, i.e. iOS's tree with the 14391 replayed LBAs folded in); 4 erases and 50 page
programs, all in sb 1048; a fresh mount loads it with the same map digest
`b9d6876bf398e101` and nothing to replay. sb 1020 (iOS) stays listed as a context. Left on
the media for iOS to boot on.

**After iOS** (user: iOS boots and works; read-only check on the rung-1 image): clean.

- iOS loaded the Linux context and committed its own on **sb 248**, weave 377607947,
  200 LBAs, tree 172 (87778 runs after 17668 replayed LBAs).
- The golden rebuild of sb 248 is identical to the media (87675 normalised runs).
- iOS's SB array lists sb 248 and sb 1048 (the Linux context) as contexts, but **sb 1048 is
  erased** on the media: iOS erases the previous context after committing, and keeps it
  listed with type 7. The same was seen for sb 456 before (listed, no context on it).
- sb 1020 (the iOS context the Linux commit kept) was erased and reused by iOS as its
  open frontier (type 4, 670 valid); sb 456 now holds closed data.
- 20000 random mapped LBAs read with matching metadata; LwVM System, Data, Linux.

### Rung 6, round 2, 2026-11-15 — iOS booted on it; rung 6 passed

`action=commit` again: Linux context on sb 1048 (erased by iOS, listed type 7 in its array;
the writer's reconcile took it as erased), weave 377625819, 200 LBAs, tree 172 (87847
runs with iOS's 17668 replayed LBAs folded in); 4 erases, 50 programs, all in sb 1048; a
fresh mount loads it with digest `82d5ca02ff7918d1`, nothing to replay. iOS's sb 248 kept.

**After iOS** (user: iOS boots and works; read-only check): clean.

- iOS loaded the Linux context and committed its own on **sb 467**, weave 377632440,
  200 LBAs, tree 172 (87862 runs after 18455 replayed LBAs); golden rebuild identical
  (87750 normalised runs).
- Its SB array lists sb 467 and sb 1048 (the Linux context) as contexts; sb 1048 is erased
  on the media, as in round 1.
- sb 248, iOS's context of round 1 that the Linux commit kept, is listed type 1 (await
  erase, valid 0) and already holds new data (first weave 377647424, after the context).
- 20000 random mapped LBAs read with matching metadata; LwVM System, Data, Linux.

**Rung 6 passed (2026-11-15):** iOS booted twice on a Linux context, took it as its own,
committed after it, and its contexts rebuild identically from their tables. What iOS does
with the older contexts: it erases the previous one after its own commit and keeps it
listed as type 7 until the next commit, then lists it type 1 and reuses it.

### Rung 7, 2026-11-15: real reclaim + commit, iOS booted on it — passed

**Writer bug found offline first** (fixed in 2139ecf): reclaim marks its victims
await-erase with nothing valid and queues them for erase after the commit, and
`take_spare()` counted such SBs as spares. On the `w0c` overlay the commit put its
context on sb 280, one of the victims, and erased it straight after; the remount fell
back to the older context and replay, so the map still looked right. Spares now skip the
erase queue, a commit refuses to erase its own new context, and the rung-7 test requires
the remount to load the new context with nothing to replay.

On the device (`devices/a5/tools/nand-rung7`, then the rung-6 check):

- Context before: iOS's sb 467. Victims (fewest valid): sb 376, 876 (12 valid each),
  296, 401 (13 each); 50 live LBAs to move.
- `sftl_gc()`: move, PAD fill and BTOC, commit to **sb 427** (weave 377655188, 200 LBAs,
  tree 173), erase the victims. Watcher: 24 erases (4 victims, frontier, context) and
  1074 page programs, all in blocks erased in the run; 0 refused.
- Remount: context sb 427, nothing to replay; the 50 moved LBAs read the same hash, type
  and user from outside the victims; the map digest over every other LBA unchanged
  (`951f97539a681bf6`, 1187708 mapped); the four victims read erased in full.
- Rung-6 check: golden of sb 427 identical (88026 runs), 20000 random LBAs read, LwVM fine;
  sb 467 (iOS) stays as the second context.

Left on the media for the iOS boot.

**After iOS** (user: iOS boots and works; read-only rung-6 check, clean):

- iOS loaded sb 427 and committed its own context on **sb 401**, one of the four victims
  Linux had erased; golden rebuild identical (87926 runs, tree 173 LBAs).
- **sb 256, the Linux GC superblock** (first weave 377651096, 50 moved LBAs, PAD fill to
  capacity, BTOC nb 4): iOS lists it closed (type 3), 50 valid, BTOC flags `0x8`. iOS took
  a Linux data superblock with its PAD fill as an ordinary closed one (decision D1).
- Victims sb 376 and 876 are iOS's open frontiers now (type 4); sb 427 (Linux context)
  is erased and listed type 7, as in rung 6.
- **sb 467**, iOS's earlier context, is erased and listed with **type 2**, a value not
  seen in `w0c` (§4.4 lists 1, 3, 4, 5, 7, 8, 9). Meaning unknown; to note for §9.
- 20000 random mapped LBAs read with matching metadata; LwVM System, Data, Linux.

### Rung 8a, 2026-11-15: ext4 on the Linux volume through `apple_ftl`

`devices/a5/tools/nand-rung8/rung8a.sh` on the rung-1 image (`apple_ftl auto_mount=0
enable_write=1 write_volume=Linux verify_every=1`); `e2fsck` 1.47.4 from Alpine 3.24
armv7 packages, run from `/tmp`. Log in `/srv/nand-dumps/rung8` on the NFS root.

- Disks: `ftl0`, `ftl0p0` System and `ftl0p1` Data read-only, `ftl0p2` Linux writable. A
  write of System's own block 1000 back to it is refused.
- `e2fsck -fn` before: clean. ext4 mounted `rw,noatime,commit=30`, 14 % used, all writes in
  `/rung8-test`: 128 MiB in 25 s with sync (5.1 MiB/s, every page verified); 2000 files of
  1–16 KiB in 16 s; 300 write + fsync + rename + directory fsync, 49 ms each.
- **Two reclaims ran by themselves** (spares under the floor of 6): 4 victims and 52 live
  LBAs, then 4 and 55, each with its commit (sb 1005, sb 475); the unmount checkpoint
  committed sb 30.
- Remount: context sb 30, nothing to replay; `e2fsck -fn` clean; all 2301 files match
  their SHA-256.

**After iOS** (`tools/nand-rung8/check.sh`, read only): iOS committed its context on sb 876
(golden identical, 88219 runs; 1225604 mapped, +37896 since rung 7); the Linux contexts
sb 30 and sb 475 are erased, listed type 7 and type 1. 20000 random LBAs read;
`e2fsck -fn` of the Linux volume clean; all 2301 files of rung 8a match.

### Rung 8b, 2026-11-15: unmount without checkpoint, replay

`tools/nand-rung8/rung8b.sh`, with the new test parameter `apple_ftl.skip_checkpoint`
(commit 372a906): 32 MiB and 500 small files in `/rung8-test/crash`, sync, ext4 unmounted,
FTL unmounted **without** the final checkpoint, so the Linux frontier stays open with no
BTOC. Remount: replay 27293 LBAs (iOS's 18157 plus the open Linux frontier by metadata
scan); `e2fsck -fn` clean; the 501 new files and the 2301 of rung 8a all match. The next
unmount committed sb 248; a remount after it replays nothing.

**After iOS** (read only): iOS committed sb 332 (golden identical, 88125 runs; 1238888
mapped); the Linux context sb 248 is erased and listed type 7. 20000 random LBAs read;
`e2fsck -fn` clean; the 2301 files of 8a and the 501 of 8b all match.

**Rung 8 passed (2026-11-15):** ext4 writes on the Linux volume through `apple_ftl`,
fsync and rename load, crash (no checkpoint) replay, checkpoint rotation (contexts on sb
1005, 475, 30, 248), automatic reclaim, unmount checkpoint, and iOS booted after each
part. The test files stay in `/rung8-test` on the Linux volume (about 165 MiB).

### Rung 9, 2026-11-15: root on the NAND Linux volume — passed

Image `tools/nand-rung9/build.sh` (commit b31af08): rung-1 kernel, loader and DTB; overlay
with stage 1's `/init` plus a NAND-root branch (driver modules from `/lib/nand`, FTL writable
for `Linux` only, `ftl0p2` mounted ext4 `rw,noatime,commit=30`, `switch_root` to the root's
own `/sbin/p105-stage2`; any failure falls through to NFS). Nothing is copied into the root.
`verify_every=1`.

- Boot: root `/dev/ftl0p2` 10.6 s after start; 64 MiB + sync in 13 s, 500 small files.
- Round trips Linux (NAND root) → power-off → iOS → Linux (NAND root), three times: every
  test file and the markers written before each power-off are intact; iOS's contexts (sb 972,
  825, 972 again) rebuild identically; random reads clean.
- `p105-stage2` turns `reboot` into a power-off on this iPad. The FTL's reboot notifier
  also runs on power-off, but its messages are lost with the ssh link. Evidence that the
  shutdown checkpoint is written: each time iOS's next context lists as its previous context
  an SB other than the one the Linux session had loaded (sb 304 after the session that loaded sb 972;
  sb 66 after the one that loaded sb 825), as when iOS booted on a Linux context in rungs 6–8, where it listed
  exactly that Linux context. Not proven directly: that needs a boot into Linux right after
  the power-off with no iOS in between.
- Fourth round (meant to be Linux → Linux, but iOS ran in between again): iOS's context
  sb 300 lists **sb 66** as its previous context. In the Linux session before the power-off
  sb 66 read erased (`ff`), and a Linux commit in that session would have kept sb 419 (the
  context it loaded), not sb 66. So the context on sb 66 was written at the Linux power-off
  and iOS booted on it; the only other reading is that iOS committed twice in one session,
  which none of the sessions observed in rungs 6–9 did.
- A replay of 61052 LBAs (iOS wrote about 230 MiB after its context sb 419 without
  committing again) was complete: the replay log held only 16 entries, and the 17th SB
  (sb 972, 4108 LBAs) was applied but not logged. The log is now 64 entries.
- **Tree length rule corrected** (`sftl_ctx_tree_lbas`): iOS's sb 300 holds 176 used tree
  LBAs in exactly 176, with no empty LBA. The rule is `max(32, roundup(used, 16))`, which fits
  all iOS contexts seen (26 → 32; 172, 173, 174, 176 → 176). The earlier `used + 1` rule
  gave 192 here; iOS accepted those Linux contexts, but the golden rebuild of sb 300 now
  matches only with the corrected rule.
- Why iOS always ran in between (user, 2026-11-15): with the cable in, a power-off from
  Linux is followed by the iPad powering on again, and DFU is entered from the Apple logo,
  by when iOS's FTL has started. Each round shows exactly one iOS commit, made at that early
  boot, listing as its previous context the one it booted on. So sb 66 being that previous
  context is the direct trace of the Linux power-off checkpoint.

**Rung 9 passed (2026-11-15, user decision on this evidence):** root on the NAND Linux
volume through the clean driver, four power-off round trips through iOS, every test file
and every marker written before a power-off intact, the power-off checkpoint seen through
iOS's next context, iOS's contexts golden-identical.

### Rung 10, 2026-11-15: sustained speed and long reads — passed

`tools/nand-rung9/rung10.sh` on the NAND root (rung-9 image, corrected modules,
`verify_every=16`):

| | measured | spec §7.5 target |
|---|---|---|
| sequential write, ext4, 512 MiB zeros, fsync | 8.0 MiB/s | 8–10 MiB/s |
| sequential write, 256 MiB random, sync | 7.3 MiB/s | |
| sequential read, 512 MiB file, cold cache | 9.6 MiB/s | 8 MiB/s |
| 4 KiB write + fsync, 300 times | mean 13.4 ms, p95 16.1, max 30.7 | ≤ 40 ms |
| whole `ftl0` read (16 GB), twice | 698 s and 695 s, 22 MB/s, no error | |

The write figures include five automatic reclaims during the 768 MiB written (about one
per 48 MiB: 4 victims of 56–60 live LBAs, then a commit, about 2 s each). Each reclaim
ends in a commit, and a commit closes the frontier with PAD, so a partly used superblock
is filled with PAD each time. On this media, where spares sit at the floor, that is about
one superblock of PAD per 48 MiB written. Not a correctness issue; a writer tuning to look
at (more victims per reclaim, or fewer commits).

**After a full iOS boot** (user let iOS start completely and used it): the NAND root boots
again; iOS's context sb 208 is golden-identical (88673 runs; 1417128 mapped); 20000 random
LBAs read; all rung-9 files and the last marker intact.

**Ladder of spec §8.2 complete (2026-11-15):** rungs 1–10 passed with the clean-room
driver.

Still open before any publication: bad-block handling on a program error (done
2026-11-16, below), the reclaim PAD cost above, SB type 2 (seen once on an erased former
context), and the §9 items not touched by the ladder.

### Daily boot on the clean driver: nand-rw kernel, cable test (2026-11-15)

`tools/nand/build-nand-rw.sh` (from main 822c260, adapted in 87f7a4c): kernel
`6.12.0-nand-rw`, `CONFIG_APPLE_PPN=y`, `CONFIG_APPLE_SFTL=y`, `CONFIG_APPLE_SFTL_WRITE=y`,
h2fmi off; stage 1 hook `p105-nand-root` (§7.4 interface: `hold_probe`, bind
`31200000.ppn`, `mount`, write gates). PC-less kernelcache SHA-1
`3572487621b2942053d08e17824d4184bf2a8171` (old driver's: `fd95adeb…`, kept in
main's `build/pcless-cablefree/out.old-driver`).

Cable boot of the bundle (`FLASH_OUT=build/nand-rw-boot`): the hook found the AIC timer as
the tick, released `hold_probe`, bound the controller, mounted the FTL (context sb 375,
**0 replayed**: the power-off checkpoint of the session before, with no iOS run in
between), root `ftl0p2` rw, `switch_root` to `/sbin/p105-nand-init` at 8.1 s; System,
Data and `ftl0` read-only. 64 MiB + sync in 6.8 s (9.4 MiB/s, verify every 16); 300 small
files; all new and older test files match after dropping caches; XFCE (`desktop`) up.

Installed from iOS with `app/install-dev.sh` (2026-11-15): the app's status shows
`/kernelcache` = `3572487621b2942053d08e17824d4184bf2a8171` as wanted; ramdisk, devicetree,
kloader and iBEC unchanged.

- Boot from the app with the cable out: Linux and the desktop came up (user).
- iOS after it: `fsck_hfs -l` (live, no write) of System and Data: both "appears to be OK".
  64 System blocks of 4 KiB spread over the volume and its first 16 MiB hashed from iOS
  through `/dev/rdisk0s1s1` (`tools/nand/ios-system-samples.sh`).
- Second boot from the app: context sb 796, 18527 replayed (iOS's writes after its
  commit); AIC timer tick; all earlier test files match; the **64 System blocks read through
  `/dev/ftl0p0` equal iOS's**. 32 MiB + 200 files written in 4.3 s. `reboot` → iOS.
- iOS after it: `fsck_hfs -l` of System and Data clean again. Of the 4096 blocks of System's
  first 16 MiB, 2050 differ between the Linux read and iOS's read after its boot: the
  volume header (block 0), the journal info block and journal (33, 34–2081) and two blocks
  of the allocation bitmap (3, 4; the file is blocks 1–32). iOS keeps System mounted
  read-write (the volume is marked not unmounted) and updates exactly these; every other
  block is equal. This settles the rung-1 open item of a sample compare with the iOS view.
- Third boot from the app: the second session's 201 files and marker, and all earlier
  test files, match. iOS's context is again on sb 796 (a new one, weave 378509180, SB reused)
  and lists **sb 53** as its previous context: the context the nand-rw kernel wrote at the
  power-off of the second session (which had loaded the older sb 796). 41156 LBAs replayed
  (iOS's session after its commit); golden of sb 796 identical. An open SB (sb 748, 40 LBAs,
  newest weaves) is listed with **type 2** in iOS's array: type 2 also marks an open data SB,
  not only an erased former context.
- Test files removed (`/rung8-test`, 351 MiB). Then, on the NFS rung-1 image with the
  driver read-only (context sb 1042, 18263 replayed): `e2fsck -fn /dev/ftl0p2` of the
  unmounted Linux volume is clean (18504 files, 376737 of 2097152 blocks).
- Not checked here: long use.

### Program and erase failures (spec §4.9), 2026-11-16

Writer: c2958e1; test injection in `apple_ppn` (`inject_program`, `inject_mode`,
`inject_erase`, off by default): c0d5a20. Offline, `nandtest --faults`: 8 cases (program
not done, done then failed, garbage with status 0x51, a second failure while moving, BTOC
failure, context failure, erase failure, failure then a crash before commit), 47 checks on
each of `w0c` and `r6pre`, all pass; full offline suite 55/55.

Device, NFS rung-1 image, `tools/nand-faults/faults.sh`: ext4 on the Linux volume,
`verify_every=1`, three sets of 64 MiB + 300 small files:

- A, page not programmed (bank 2 block 426 page 50): sb 426 given up, 128 LBAs of the
  failed batch and 772 more moved, commit on sb 562.
- B, page programmed then reported failed (bank 2 block 852 page 57): sb 852 given up,
  128 + 876 LBAs moved, commit on sb 835.
- C, erase failure (bank 0 block 350): sb 350 not used for the session; writes went on.
- Seven commits in all during the writes; unmount with checkpoint; fresh mount on context
  sb 473, 0 replayed; `e2fsck -fn` clean (19411 files, 427237 of 2097152 blocks); all 903
  files match after the remount.
- iOS booted on it: `fsck_hfs -l` of System and Data clean. Then, NFS image, read-only:
  iOS's context sb 480 (weave 378838598, tree 175 LBAs), golden identical, 3116 replayed,
  2000 random LBAs read right; **sb 852 and sb 350 erased (type 9)**; **sb 426 erased and
  reused by iOS** (closed, type 3, its own weave 378698430); the Linux context sb 473 erased
  and still listed as type 7, as at rung 6. No grown-bad entry was expected (the failures
  were injected, the blocks are good). Linux volume: `e2fsck -fn` clean, same counts, 903
  of 903 files match.

## Decisions the spec left open

**D1. Closing a partly written superblock.** *Rung 7: iOS accepted a Linux SB closed
this way (sb 256, 50 data VBAs and PAD to capacity) as an ordinary closed SB.* The spec says "write the BTOC in the
last nb VBAs" and does not say what happens to the VBAs between the last data
and the BTOC. iOS leaves no hole `[DUMP]`, and pages of a block go in ascending
order. The code fills the rest of the data area with PAD slots at close, so a
Linux SB has the same shape as an iOS one. PAD slots take a page program each:
at most about 1.3 s for a whole SB, at checkpoint only. The alternative, leaving
the pages erased with a shorter BTOC, is unproven against iOS. Tested on the device at rungs 3 and 7.

**D2. "Status poll" after read/parameters.** §2.5 and §2.1 read "status poll"; §1.5
defines both a bare poll and an operation status (`0x7d77` + poll). `[iBEC 0x9ff0758a]`
uses operation status for the matching get-feature sequence. The driver uses
operation status for get-feature, parameters and read. Unconfirmed on the device
for parameters and read until rung 1.

**D3. Tree overflow.** *Revised at rung 4:* iOS writes trees past 32 LBAs (above),
so the writer does too, up to 512 LBAs, and compacts only past that. The original
decision follows. The spec fixes the tree at 32 LBAs (16352 runs) and does not
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

- **Bad-block handling**: done and checked with iOS (section above, 2026-11-16). A real
  bad block was not met; only injected failures were tested.
- All facts of spec §9 except the transmit `CONFIG` (settled at rung 2) remain open: the meaning of base words,
  the SB type values for retired SBs, overlapping programs on one CE, read-clock
  sweeps, iOS's behaviour on an open Linux frontier, power loss during a batch,
  iOS's reaction to Linux DELETE tokens, the historic boot loop. None was settled
  by this work.
