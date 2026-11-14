#!/bin/sh
# Ladder rung 8, part b: writes, then an unmount without the final checkpoint
# (apple_ftl.skip_checkpoint=1, as a crash would leave the frontier open);
# remount: replay must bring the writes back; then the unmount checkpoint.
set -u
OUT=${OUT:-/srv/nand-dumps/rung8}
M=/mnt/nandlinux
T=$M/rung8-test
E2="env LD_LIBRARY_PATH=/tmp/e2fs/usr/lib /tmp/e2fs/sbin/e2fsck"
P=/sys/module/apple_ftl/parameters
log() { echo "$(date +%T) $*" | tee -a "$OUT/log.txt"; }
fail() { log "FAIL: $*"; exit 1; }
mkdir -p $M
dmesg -c >/dev/null
insmod /tmp/apple_ppn.ko allow_write=1 || fail apple_ppn
insmod /tmp/apple_ftl.ko auto_mount=0 enable_write=1 write_volume=Linux verify_every=1 || fail apple_ftl
echo 1 > $P/mount || fail "FTL mount"
dmesg | grep "apple_ftl: context" | tail -1 | tee -a "$OUT/log.txt"
mount -t ext4 -o rw,noatime,commit=30 /dev/ftl0p2 $M || fail "ext4 mount"
rm -rf $T/crash; mkdir -p $T/crash
dd if=/dev/urandom of=$T/crash/big.bin bs=1M count=32 2>/dev/null
i=0; while [ $i -lt 500 ]; do
	dd if=/dev/urandom of=$T/crash/s$i bs=1024 count=$(( (i % 8) + 1 )) 2>/dev/null; i=$((i + 1))
done
sync
(cd $T/crash && find . -type f | sort | xargs sha256sum) > "$OUT/crash-sums.txt"
umount $M || fail umount
echo 1 > $P/skip_checkpoint
echo 0 > $P/mount
echo 0 > $P/skip_checkpoint
log "unmounted WITHOUT checkpoint ($(wc -l < "$OUT/crash-sums.txt") new files)"
dmesg | grep -E "skipped|committed|reclaim" | tail -5 | tee -a "$OUT/log.txt"

echo 1 > $P/mount || fail "FTL remount"
dmesg | grep "apple_ftl: context" | tail -1 | tee -a "$OUT/log.txt"
$E2 -fn /dev/ftl0p2 > "$OUT/fsck-crash.txt" 2>&1; log "e2fsck -fn after replay: exit $?"
mount -t ext4 -o ro /dev/ftl0p2 $M || fail "ro mount"
(cd $T/crash && sha256sum -c "$OUT/crash-sums.txt" 2>&1 | grep -vc ": OK$") > /tmp/bad
log "crash files not OK: $(cat /tmp/bad)"
(cd $T && sha256sum -c "$OUT/sums.txt" 2>&1 | grep -vc ": OK$") > /tmp/bad
log "rung-8a files not OK: $(cat /tmp/bad)"
umount $M
echo 0 > $P/mount
dmesg | grep -E "committed" | tail -1 | tee -a "$OUT/log.txt"
log "unmounted with checkpoint"
echo 1 > $P/mount
dmesg | grep "apple_ftl: context" | tail -1 | tee -a "$OUT/log.txt"
echo 0 > $P/mount
rmmod apple_ftl; rmmod apple_ppn
log "8b done"
