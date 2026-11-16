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

## Gaps (see p105-nand-spec-gaps.md, "Volume creation")

V1–V6 there; the plan cannot be finished without answers to V1–V3.
