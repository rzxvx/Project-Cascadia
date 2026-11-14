#!/bin/sh
# Read-only check after an iOS boot (rung 8): FTL check (ppn_rung6 action=check),
# then the Linux volume read-only: e2fsck -fn and the SHA-256 list of rung 8a.
set -u
OUT=${OUT:-/srv/nand-dumps/rung8}
M=/mnt/nandlinux
E2="env LD_LIBRARY_PATH=/tmp/e2fs/usr/lib /tmp/e2fs/sbin/e2fsck"
log() { echo "$(date +%T) $*" | tee -a "$OUT/log.txt"; }
mkdir -p $M
dmesg -c >/dev/null
insmod /tmp/apple_ppn.ko || exit 1
insmod /tmp/ppn_rung6.ko action=check ${WATCH:+watch=$WATCH}
log "FTL check: $(cat /sys/module/ppn_rung6/parameters/result)"
rmmod ppn_rung6
dmesg -c | grep -E "rung6|tree of" | sed "s/^\[[ 0-9.]*\] //" | tee -a "$OUT/log.txt"
insmod /tmp/apple_ftl.ko auto_mount=1 || exit 1
dmesg | grep "apple_ftl: context" | tail -1 | tee -a "$OUT/log.txt"
log "ftl0p2 ro=$(cat /sys/class/block/ftl0p2/ro)"
$E2 -fn /dev/ftl0p2 > "$OUT/fsck-check.txt" 2>&1; log "e2fsck -fn: exit $?"
mount -t ext4 -o ro,noload /dev/ftl0p2 $M || exit 1
(cd $M/rung8-test && sha256sum -c "$OUT/sums.txt" 2>&1 | grep -vc ": OK$") > /tmp/bad
log "files not OK: $(cat /tmp/bad) of $(wc -l < "$OUT/sums.txt")"
if [ -f "$OUT/crash-sums.txt" ]; then
	(cd $M/rung8-test/crash && sha256sum -c "$OUT/crash-sums.txt" 2>&1 | grep -vc ": OK$") > /tmp/bad
	log "crash files not OK: $(cat /tmp/bad) of $(wc -l < "$OUT/crash-sums.txt")"
fi
umount $M
rmmod apple_ftl; rmmod apple_ppn
log "check done"
