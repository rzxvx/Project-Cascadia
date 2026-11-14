# NAND ladder rung 2: reserved blocks

Test module for step 2 of the device ladder in
`devices/a5/docs/research/p105-nand-spec.md` §8.2: program, erase and read
cycles on the six reserved blocks the VFL tables mark available (bank 0 block
1054, bank 1 blocks 1054–1056, bank 3 blocks 1055–1056). It refuses every other
block, and refuses a block that does not read fully erased before it starts.

Build (external module against the A5 kernel tree, after building the driver
in `devices/a5/patches/files/drivers/mtd/nand/apple` so its `Module.symvers`
exists):

    make -C <kernel> M=$PWD ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- \
         KBUILD_EXTRA_SYMBOLS=<driver dir>/Module.symvers modules

On the device, with `apple_ppn` loaded with `allow_write=1` and the FTL not
loaded:

    insmod ppn_rung2.ko action=survey; rmmod ppn_rung2
    insmod ppn_rung2.ko action=page  bank=0 block=1054 slc=0; rmmod ppn_rung2
    insmod ppn_rung2.ko action=cycle bank=0 block=1054 slc=1; rmmod ppn_rung2

The module always loads, so that busybox `insmod`, which repeats a failed load
through a second system call, cannot run an action twice. The outcome is
`/sys/module/ppn_rung2/parameters/result` (0 passed, negative errno failed) and
the kernel log, lines starting `rung2:`; then `rmmod ppn_rung2`.
`survey` prints a checksum of each reserved block and of its neighbours, to be
compared before and after the writes.

`apple_ppn.tx_shape` selects the page transmit: 0 (default) one start per
4112-byte unit, four sectors of 1028 bytes, as the receive path; 1 bursts of up
to 1024 bytes, as the feature transmit.
