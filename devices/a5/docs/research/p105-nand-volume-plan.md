# Creating the Linux volume with the clean driver (plan, 2026-11-16)

Goal: a user with a stock jailbroken iPad mini 1 (iOS 8.4.1, two LwVM volumes System
and Data) gets a third LwVM volume `Linux` with ext4 and the desktop root on it, from
`./cascadia a5 …` on the computer. Today the clean driver only reads LwVM (§6 of the
spec); the user's own three-volume layout was made with earlier tools.

Spec rules this follows (§6): the new layout is checked offline against iOS's own LwVM
validator before any write; Data is shrunk with a native HFS shrink **first**, the header
is written only after; header writes go backup (LBA 4095) then primary (LBA 0), through
the FTL, each verified; header changes are a separate deliberate operation, never done by
the running writer. Capacity rule: a partition ends at most at 15854493696.

## Flow (one command, steps resumable, each device step asks first)

1. **Check (iOS over USB, read-only).** Model and build; read LwVM at LBA 0 and 4095 of
   `/dev/rdisk0` (both copies equal, CRC valid, two volumes); Data's size and free space;
   battery. Refuse on anything unexpected (three volumes already present: report it and
   stop).
2. **Plan (computer, offline).** Linux size: default 8 GiB, at most what Data can give with
   a safety margin and what the capacity rule allows. Build the new header: Data's end
   lowered, a `Linux` entry added, chunk map updated, CRC. Run iOS's validator on it
   (emulated, 12H321 kernelcache from the IPSW the user already has). Show the plan.
3. **Shrink Data (iOS).** Native HFS shrink of the mounted Data volume to the planned size,
   then reboot iOS and check (`fsck_hfs -l`; rc 8 right after a large shrink is expected
   until a clean restart, §5).
4. **Write the header (Linux, clean driver).** Boot the NFS image; a one-shot tool reads the
   header from the FTL, checks it is still the one planned from, writes backup then
   primary through the FTL, reads both back, commits. Nothing else is written.
5. **iOS boots on it.** iOS up, three volumes seen, `fsck_hfs -l` of System and Data clean.
6. **Fill the volume (Linux).** `mkfs.ext4` on `ftl0p2`, copy the desktop root, install
   the PC-less boot files through the app as today.

Undo (give the space back to iOS) is the same in reverse: remove the entry, then grow Data.
Not in the first version unless asked.

## Testing without losing the user's Linux volume

- Offline: from the two-volume header of the `w0c` dump and today's Data size, the builder
  must reproduce today's three-volume header (CRC `d32fc8ee`) except the random unique
  GUID, or explain each difference. The validator must accept every header the tool would
  write and refuse one past the capacity rule (§8.1).
- Device, without changing the layout: steps 1–2 read-only on iOS; step 4 rewriting the
  identical current header (backup, primary, verify, commit), then iOS boot.
- A real creation needs an iPad with two volumes: either the user's after an undo (the
  Linux root is lost then; back it up first), or another device.

## User decisions (2026-11-16)

- Gaps V1–V6 go to the spec session; no code before the spec covers V1–V3.
- A separate command (working name `./cascadia a5 nand-volume`); `install` may call it later.
- Real test on the user's iPad: back up the Linux root to the computer, give the space back
  to Data (undo), create the volume again with the new command, restore the root.
  So undo (V6) is in the first version.

## Gaps (see p105-nand-spec-gaps.md, "Volume creation")

V1–V6 there; the plan cannot be finished without answers to V1–V3.

## Status (2026-11-16)

Spec §6 rewritten by the spec session (main 02a3a11, merged here). Built on it:

- `tools/nand-volume/lwvm.py`: plan create/remove, rules of §6.2 in Python, iOS's own
  validator through `lwvm_validate.py` (the spec session's §6.6 harness; needs Unicorn and
  the 12H321 kernelcache from `./cascadia a5 firmware`). `selftest` runs the §8.1 cases:
  22/22, including the one-step create from `w0c` (CRC `f45e7c76`) and the grow of the
  96 MiB header reproducing today's header byte for byte.
- `tools/nand-volume/lwvmw.c` + `ppn_lwvmw.ko`: the header write of §6.4 step 3. Offline on
  `w0c` (create 8 GiB) and `r6pre` (restore `w0c`): dry run, write, rerun (commit only),
  write back, finishing a half-written header, refusing a stale one: 12/12.
- `tools/nand-volume/ios/hfs-resize.c` → `cascadia-hfs` (armv7, ldid): `info`, `resize N`.
- `./cascadia a5 nand-volume status|create|remove|backup|fill|build`.

Not run on the device yet. Device sequence for the user's iPad (each write step asks):
1. daily boot: `backup` (tar of the root to the computer; read-only on the NAND);
2. iOS: `remove --saved build/lwvm-fixtures/two-w0c.bin` (plan only);
3. Linux image: `remove` (header = `w0c`, commit);
4. iOS: `remove` (HFS grow, never run before), clean restart, `remove` (check);
5. iOS: `create 8G` (shrink), clean restart, `create` (check);
6. Linux image: `create` (header write, commit);
7. iOS: `create` (check);
8. Linux image: `fill <backup tar>`; then the daily boot.

## Device run on the user's iPad (2026-11-16)

1. `backup`: `linux-root-20261116-142332.tar`, 1.3 GB, 19960 entries (= files on the root),
   spot SHA-256 checks equal.
2. iOS `status`: today's header (CRC `d32fc8ee`, iOS's validator 0); Data HFS 1230568 blocks,
   953925 free. `remove --saved two-w0c.bin`: plan = `w0c` byte for byte.
3. NFS image, `remove`: context sb 701 (22127 replayed); dry run passed; LBA 4095 then LBA 0
   written and read back; committed on sb 531; fresh mount: both copies = `w0c`, System 133
   and Data 813 chunks.
4. iOS booted on the `w0c` header (ssh came up only after a while). `fsck_hfs -n -l` System
   exit 0. **Data exit 8 before the grow**, unlike §6.5 step 3: "Invalid volume free block
   count (should be 3029500 instead of 956924)", bitmap "orphaned blocks" and
   "under-allocation", "minor repair". HFS header: total 1230568, free 956902, allocation
   file 417792 bytes (covers 3342336 blocks); the bitmap read raw has **no bit set past block
   1230568**, and 956715 free below it. 3029500 = free + every block up to 3303144 (the HFS
   size after the 96 MiB step of 2026-10-29): fsck judges the free count against a size that
   is neither the HFS's nor the container's. Taken as an artefact of the smaller HFS in the
   larger container (user decision to grow).
   (A first exit 8 was a test error: fsck's output redirected to `/tmp` on Data itself, which a
   live verify refuses.)
   Grow: `cascadia-hfs resize 3327720` → `fsctl` 0, errno 0, under a second; HFS header and
   alternate header 3327720, free 3053024; `fsck_hfs -n -l` Data exit 0 right after; `df`
   12.7 GiB. **First HFS grow on this device: works.**
5. Clean restart: `nand-volume remove` → Data 3327720 blocks, fsck exit 0 (System and Data):
   **removal done**. Spec §6.5 step 3 corrected (main 78abaeb): before the grow, Data's
   exit 8 is accepted only with exactly the size-mismatch findings, `F' = F + (S − N)` with
   `N ≤ S ≤ container`, no bitmap bit at or past `N`. `hfs_undo_check.py` implements it
   (today's findings accepted; an extra finding, a container too small, a bit past `N`, or
   output redirected to Data refused). fsck output is now captured as two streams on the
   computer.
6. iOS `create 8G` (`LINUX_UUID` = the old one): plan = spec §8.1 one-step create from `w0c`
   (CRC `f45e7c76`, SHA-256 `afd27ea1…`); fsck exit 0, 3052939 free; `fsctl` shrink to
   1230568: 0, under a second. The raw read right after showed no alternate header; after a
   `sync` it was there with 1230568 (the tool now syncs first). Clean restart: fsck exit 0,
   both headers 1230568.
7. NFS image `create`: context sb 840 (20172 replayed); dry run passed; LBA 4095, LBA 0 written
   and read back; committed on sb 3; fresh mount: System 133, Data 301, Linux 512 chunks.
8. iOS on the new header: header read from iOS = plan (CRC `f45e7c76`); `/dev/disk0s1s3`
   present; fsck exit 0 on System and Data; `df` Data 4.7 GiB. **Creation done.**
9. NFS image `fill linux-root-20261116-142332.tar`: `mke2fs -t ext4 -L CascadiaLinux`, tar
   unpacked through the FTL. (The tool's own e2fsck ran after the FTL was detached and did not
   check; fixed.) Read-only check after: context sb 25, 0 replayed; `e2fsck -fn` clean
   (19716 inodes, 403010 blocks); 19960 entries as in the backup; the four sampled files have
   the same SHA-256; owners kept.
