# P105 NAND: raw erase/program from Linux, UBI + UBIFS on the chip (2026-10-26/27)

Lab notes from liquidmetal-Hg's iPad mini 1 (two Hynix PPN packages, signature
`50 50 50 4e 4e 01 01 05`), first reported in issue #29. Kept as research, as
asked there: this was a **raw** write path inside a small window of blocks, not
a write path through iOS's FTL, and there is no free space on the chip for it in
real use (iOS rewrites its blocks at every boot). The command sequences
themselves are in [`p105-nand-spec.md`](p105-nand-spec.md) §2.6 and §2.7. This
note only adds what is not there: what went wrong, the timings, UBI/UBIFS on
the PPN, and how a test range was put back.

Tested from a kernel built on the tree as it was on 2026-10-26 (the old
read-only `mtd-apple-h2fmi.c` plus `_erase`/`_write_oob`). All tests were on
bus 0, CAU 0, blocks 589 (single block) and a 32-block window at 2400 MiB. A
raw dump of the range was taken right before each test and compared afterwards.

## What wedged the bus

- **Stopping an erase after `79` + ready poll** left the PPN answering `0x51`
  and both buses dead. The sequence must be carried through to `77 7D` and the
  closing `77`, even when the ready status shows unexpected bits.
  `[DEV 2026-10-26/27]`
- **`77 7D` after a program** (copying the erase sequence) timed out and wedged
  the bus. The program ends with `79` + ready poll only. This matches the
  firmware's sequencer macro `0x11000605` (table at `iop_fw` offset 0x130b8,
  loaded into FMI+0xc1000), which expands to `79`, delay, ready poll and nothing
  more. `[DEV 2026-10-26/27]`
- After either, only a fresh boot through iBEC brought the buses back.
  `nandctl reset` / `recover` did not help, and the other bus went quiet
  afterwards too (cause unknown). A NAND reset also drops the PPN to SDR and
  loses iBEC's DDR setup (see "Trap: two processes on one bus" in `p105-nand.md`).
- Status values seen on success: ready poll after an erase `0x44` every time
  (`0x04` meaning unknown, not an error); after a program `0x62`. Bits `0x20`
  and `0x02` unexplained.

## Row address bytes: 3 worked here

Both erase (`6A <row> D7`) and program (`8A <row>`) were sent with **3 row
bytes** (`ADDRNUM = 2`), row = `page | block << 8 | cau << 19`. Spec §2.4 says
4 bytes on PPN >= 1.5.0 `[iBEC 0x9ff08502]`. With 3 bytes, block 589 was erased
and programmed and read back with 0 differences, and the neighbouring blocks on
all four dies were unchanged against the raw dump. The 4th byte would have been
0 for every row used here, so this does not show what the chip does with 3
bytes for a row above `0xffffff`. Worth knowing if anything ever sends 3.

## Timings (PIO)

| Operation | Time |
|---|---|
| Program a full block (256 pages x 16448 B, one `8A` each, ascending) | 0.6-0.7 s, about 6.6 MB/s |
| `ubiformat` of 32 PEBs (128 MiB) | 1.4 s |

## UBI and UBIFS on the PPN

Partition `mtdparts=apple-nand0:128m@2400m(ubiscratch)`, 32 PEBs of 4 MiB.

- `ubiattach -b 1`: UBI's default bad-block reserve takes 21 of 32 PEBs here
  and leaves too few LEBs for UBIFS.
- **LEB size 4,161,536 bytes** (PEB 4 MiB minus 2 pages). The kernel has no
  maximum LEB size, but `mkfs.ubifs` (mtd-utils 2.2.1) refuses anything above
  2 MiB (`UBIFS_MAX_LEB_SZ`, `libubifs/ubifs.h`). The test used the Alpine
  armhf binary with that one compare patched (`cmp r5,#0x200000` ->
  `#0x800000`, file offset 0x5cd8). Building mtd-utils with the constant
  raised is the clean way.
- Volume of 26 LEBs, UBIFS mounted (16 LEBs minimum). 45 files verified by
  sha256, 50 more files + 6 MB written on the device;
  unmount/detach/reattach/remount identical. Churn loop of 30 x 6 MB writes
  through the 63 MiB filesystem: no errors, no bad blocks, max erase counter 7,
  stable files identical. `[DEV 2026-10-27]`
- OOB: all 64 bytes exposed as free OOB; a page the PPN reports uncorrectable
  still copies its data (UBI treats a short read as a driver bug).
- No `_block_markbad`: the persistent retirement mechanism was unknown, so UBI
  goes read-only if it ever has to retire a block.

## Putting a test range back

1. Before: `nanddump -o -f snap.bin /dev/mtdN` (16384 data + 64 OOB per page),
   copy it off the device, check a second read is identical.
2. After: `flash_erase /dev/mtdN 0 0`; for each block of the snapshot that is
   not all `0xff`:
   `dd if=snap.bin bs=4210688 skip=B count=1 | nandwrite -o -s $((B*4194304)) /dev/mtdN -`
   (mtd-utils `nandwrite`; BusyBox's has no `-o`). Blank blocks stay erased.
3. `nanddump -o` again and compare: 32 blocks byte-identical, and iOS 9.3.5
   booted normally from the NAND afterwards. `[DEV 2026-10-27]`

An old dump is not a valid restore source for a range iOS has touched since:
take the snapshot immediately before the test.

## Open

- Meaning of status bits `0x04` (erase), `0x20` / `0x02` (program).
- What 3 row bytes do above row `0xffffff` (see above).
- Bad-block retirement on the media; wear under real use; DMA instead of PIO.
