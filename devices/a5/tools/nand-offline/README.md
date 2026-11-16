# Offline tests for the Swiss FTL core

Spec section 8.1 against a raw NAND dump, plus the writer on an in-memory
overlay of the dump. The dump is only ever read.

    make
    ./nandtest "<dump-dir>" --write "<nand-backup>/I1"

`<dump-dir>` holds `bus{N}-cau{C}.b{S}.bin` + `.st` for S = 0, 500, 1000 (the
`w0c` dump). The third argument is optional: a prefix of saved LwVM headers,
parsed as a check. Without `--write`, only the read-side tests run.

The writer tests enforce on the overlay what the hardware requires: erase before
program, ascending pages in a block, no touching of non-FTL blocks. The tests
cover partial pages and siblings, close and BTOC, commit, crash replay, reclaim,
tree compaction and remount.

Credit: the format and the first write path on this package are the work of
crystal-lized.
