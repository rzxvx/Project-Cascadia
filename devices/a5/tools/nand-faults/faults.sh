#!/bin/sh
# Program and erase failures on the device (spec 4.9), injected through
# apple_ppn's test parameters, under ext4 writes on the Linux volume.  Run on
# the NFS rung-1 image with apple_ppn.ko / apple_ftl.ko and e2fsck in /tmp.
# apple_ppn.ko must be built with the injection: PPN_FAULT_INJECT=1 on the
# external module build (CONFIG_APPLE_PPN_FAULT_INJECT=y in the tree).
set -u
M=/mnt/nandlinux
T=$M/bbtest
OUT=${OUT:-/srv/nand-dumps/faults}
E2="env LD_LIBRARY_PATH=/tmp/e2fs/usr/lib /tmp/e2fs/sbin/e2fsck"
PP=/sys/module/apple_ppn/parameters
FP=/sys/module/apple_ftl/parameters
mkdir -p "$OUT" $M
log() { echo "$(date +%T) $*" | tee -a "$OUT/log.txt"; }
fail() { log "FAIL: $*"; exit 1; }
write_set() {	# name
	mkdir -p $T/$1
	dd if=/dev/urandom of=$T/$1/big.bin bs=1M count=64 2>/dev/null
	i=0; while [ $i -lt 300 ]; do
		dd if=/dev/urandom of=$T/$1/s$i bs=1024 count=$(( (i % 8) + 1 )) 2>/dev/null; i=$((i + 1))
	done
	sync
}
dmesg -c >/dev/null
insmod /tmp/apple_ppn.ko allow_write=1 || fail apple_ppn
[ -e $PP/inject_program ] || { rmmod apple_ppn; fail "apple_ppn.ko built without PPN_FAULT_INJECT=1"; }
insmod /tmp/apple_ftl.ko auto_mount=0 enable_write=1 write_volume=Linux verify_every=1 || fail apple_ftl
echo 1 > $FP/mount || fail "FTL mount"
mount -t ext4 -o rw,noatime,commit=30 /dev/ftl0p2 $M || fail "ext4 mount"
rm -rf $T; mkdir -p $T

log "A: page program fails, page not programmed (after 200 programs)"
echo 1 > $PP/inject_mode; echo 200 > $PP/inject_program
write_set A
log "B: page program fails after programming (after 300 programs)"
echo 2 > $PP/inject_mode; echo 300 > $PP/inject_program
write_set B
log "C: the next erase fails"
echo 0 > $PP/inject_erase
write_set C
echo -1 > $PP/inject_program; echo -1 > $PP/inject_erase
dmesg | grep -E "injected|given up|giving it up|failed|committed|wedge|stopped" | sed "s/^\[[ 0-9.]*\] //" | tee -a "$OUT/log.txt"
(cd $T && find . -type f | sort | xargs sha256sum) > "$OUT/sums.txt"
umount $M || fail umount
echo 0 > $FP/mount
log "unmounted with checkpoint"
echo 1 > $FP/mount || fail "FTL remount"
dmesg | grep "apple_ftl: context" | tail -1 | tee -a "$OUT/log.txt"
$E2 -fn /dev/ftl0p2 > "$OUT/fsck.txt" 2>&1; log "e2fsck -fn: exit $?"
mount -t ext4 -o ro /dev/ftl0p2 $M || fail "ro mount"
(cd $T && sha256sum -c "$OUT/sums.txt" 2>&1 | grep -vc ": OK$") > /tmp/bad
log "files not OK after remount: $(cat /tmp/bad) of $(wc -l < "$OUT/sums.txt")"
umount $M
echo 0 > $FP/mount
rmmod apple_ftl; rmmod apple_ppn
log "faults done"
