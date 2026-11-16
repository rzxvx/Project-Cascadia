# NAND incident of 2026-11-16 and its recovery

## What happened

Main's unified kernel #13 (clean driver built in, root on `/dev/ftl0p2`, booted over the
cable). `ios mount` ran an old `/usr/bin/ios` from the root, which started `iosnand`: raw
`/dev/mem` access to the FMI registers (reset, reads) while `apple_ppn` owned the
controller. Kernel log (main `build/nand-volume/incident-20261116/dmesg.txt`):

- 9.46 s: mount on iOS's context sb 30, 18503 LBAs replayed (iOS's tail);
- 180.12 s: program bank 0 block 198 page 3: status 0x54;
- 180.26 s: sb 198 given up; the four spares the writer then tried, sb 453, 468, 514, 694,
  failed to erase (-110), and the writer stopped (180.81 s) **before any commit**;
- 190 s on: every read times out (bus reset timed out); the root returns I/O errors.

A stopped writer refuses writes and the checkpoint, including the reboot notifier's, so
nothing more reaches the NAND. On the media: iOS's context sb 30 and its tail; Linux's
uncommitted frontier in sb 198 with a torn page; four spares whose erase may or may not
have started.

## Why not just reboot

With the cable in, a reboot or power-off from Linux re-powers the iPad straight into
iOS's early boot (it replays and commits during the Apple logo). iOS would meet an
uncommitted Linux frontier with a torn page: never observed (spec §9).

## Fixes (nand-clean)

- 7eaef71: the replay walk marks an SB with a page that does not read back as torn; the
  writer gives a torn open SB up at start (spec 4.9: LBAs moved, type 1, nothing valid,
  commit soon), so it is never handed to iOS as an open SB. `sftl_erase_spare()`.
  `tools/nand-recover/ppn_recover.ko`. Offline replica of the incident on `w0c` and
  `r6pre`: passes.
- b54a707: `apple_ppn` claims its register windows (`request_mem_region`), so with
  `CONFIG_IO_STRICT_DEVMEM` `/dev/mem` cannot map a bound controller.
- Main (other session): `CONFIG_IO_STRICT_DEVMEM`, `iosnand`/`nandctl` refuse while
  `apple-ppn` is bound, stage 1 refreshes `/usr/bin/ios` on a NAND root.

## Recovery procedure (each device step on the user's go)

1. DFU by the buttons from the running Linux, **without reaching the Apple logo**
   (hold Power + Home until the screen goes black, release Power, keep Home about 10 s;
   if the Apple logo appears, iOS's early boot has started). Pico for pwned DFU.
2. Boot the NAND bundle with a RAM root (main: `./cascadia a5 nand-volume ram-image`,
   `build/nand-volume/ram-boot`): kernel #13, `apple_ppn.allow_write=1`, FTL not mounted.
3. Bind the controller (`hold_probe` 0, bind `31200000.ppn`); `ppn_recover.ko
   erase=453,468,514,694 dry_run=1`, then without `dry_run`: sb 198 given up, the four
   spares erased again and read back erased, commit, clean second mount.
4. Mount the FTL (`/sys/module/apple_ftl/parameters/mount`), `e2fsck -fy /dev/ftl0p2`
   (the ext4 journal was aborted), `e2fsck -fn` again, unmount (checkpoint).
5. iOS: `fsck_hfs -n -l` of System and Data.
6. The daily boot.

## Recovery run (parent session drove the device, user's go per step)

RAM-root bundle of kernel #13, controller bound by hand, `ppn_recover.ko` (9c18607):

- Dry run 1 (without `give_up`): context sb 30, 18551 replayed = the incident's 18503 plus
  48, exactly sb 198's pages 0–2 (3 rows × 4 banks × 4 LBAs). The walk read page 3 bank 0
  as erased and ended there: the program that returned 0x54 had not changed that page, so
  the SB was not flagged torn. sb 514 and 694 are type 9 in iOS's context with old data on
  page 0 (weaves below the context): **iOS's type 9 does not mean physically erased**.
- Dry run 2 (`give_up=198`): page 3 status `49 40 49 49` (bank 1 programmed, banks 0, 2, 3
  erased), pages 4–5 erased: the failed two-bus batch (bus 0 failed, bus 1 done, banks 2–3
  never reached). Bank 1's 4 LBAs were never acknowledged and are not replayed.
- Real run: sb 198 given up, 37 LBAs moved (48 slots replayed; 11 were older copies of
  LBAs written again within those pages), type 1, 0 valid; sb 453 taken and erased as the
  new frontier; 468, 514, 694 erased and read back erased; context sb 698; fresh mount: 0
  replayed, no LBA in a type-1 SB: PASS.
- Built-in `apple_ftl` mount: context sb 698, 0 replayed. `e2fsck -fy /dev/ftl0p2`:
  journal recovered, "file system was modified", 19716 files, 403010 blocks, rc 0;
  `e2fsck -fn` rc 0. Unmount: committed context sb 831.
- iOS 12H321 booted on it (20:43:27); `fsck_hfs -n -l` (output captured on the computer):
  System and Data "appear to be OK", rc 0. **Recovered.** Main merges nand-clean (region
  claim, torn-SB give-up) and rebuilds with `STRICT_DEVMEM` + `IO_STRICT_DEVMEM`; the next
  Linux boot is the daily NAND root on that kernel (not yet checked here).
