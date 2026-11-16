#!/bin/sh
# Ladder rung 8, part a (spec 8.2): the Linux volume through apple_ftl, ext4
# writes in a test directory, unmount checkpoint, remount and verify.
# Run on the rung-1 image with apple_ppn.ko, apple_ftl.ko in /tmp and
# e2fsck in /tmp/e2fs.  Results go to $OUT on the NFS root.
set -u
OUT=${OUT:-/srv/nand-dumps/rung8}
M=/mnt/nandlinux
T=$M/rung8-test
E2="env LD_LIBRARY_PATH=/tmp/e2fs/usr/lib /tmp/e2fs/sbin/e2fsck"
P=/sys/module/apple_ftl/parameters
mkdir -p "$OUT" "$M"
log() { echo "$(date +%T) $*" | tee -a "$OUT/log.txt"; }
fail() { log "FAIL: $*"; exit 1; }

dmesg -c >/dev/null
insmod /tmp/apple_ppn.ko allow_write=1 || fail "apple_ppn"
insmod /tmp/apple_ftl.ko auto_mount=0 enable_write=1 write_volume=Linux verify_every=1 || fail "apple_ftl"
echo 1 > $P/mount || fail "FTL mount"
for d in /sys/class/block/ftl0*; do log "$(basename $d): ro=$(cat $d/ro) size=$(cat $d/size)"; done
[ "$(cat /sys/class/block/ftl0p2/ro)" = 0 ] || fail "ftl0p2 not writable"
[ "$(cat /sys/class/block/ftl0p0/ro)" = 1 ] || fail "ftl0p0 (System) writable"

# A write to System must fail; the block written back is the one read, so the
# content would not change even if it went through.
dd if=/dev/ftl0p0 of=/tmp/sys-blk bs=4096 skip=1000 count=1 2>/dev/null
if dd if=/tmp/sys-blk of=/dev/ftl0p0 bs=4096 seek=1000 count=1 conv=notrunc 2>/dev/null; then
	fail "write to System accepted"
fi
log "write to System refused"

log "e2fsck -fn before"
$E2 -fn /dev/ftl0p2 > "$OUT/fsck-before.txt" 2>&1; rc=$?; log "e2fsck before: exit $rc"
if [ $rc -ge 4 ]; then
	tail -5 "$OUT/fsck-before.txt" | tee -a "$OUT/log.txt"
	echo 0 > $P/mount; rmmod apple_ftl; rmmod apple_ppn
	fail "file system not clean before the test: nothing written"
fi

mount -t ext4 -o rw,noatime,commit=30 /dev/ftl0p2 $M || fail "ext4 mount"
df -k $M | tee -a "$OUT/log.txt"
rm -rf $T; mkdir -p $T/small $T/renamed

s=$(date +%s); dd if=/dev/urandom of=$T/big.bin bs=1M count=128 2>/dev/null; sync; e=$(date +%s)
log "128 MiB written + sync in $((e - s)) s"
s=$(date +%s)
i=0; while [ $i -lt 2000 ]; do
	dd if=/dev/urandom of=$T/small/f$i bs=1024 count=$(( (i % 16) + 1 )) 2>/dev/null; i=$((i + 1))
done; sync; e=$(date +%s)
log "2000 small files + sync in $((e - s)) s"
python3 - "$T/renamed" <<'PY' | tee -a "$OUT/log.txt"
import os, sys, time
d = sys.argv[1]
fd_dir = os.open(d, os.O_RDONLY)
t0 = time.time()
for i in range(300):
    tmp = os.path.join(d, ".tmp")
    with open(tmp, "wb") as f:
        f.write(os.urandom(4096 + (i % 7) * 1000))
        f.flush()
        os.fsync(f.fileno())
    os.rename(tmp, os.path.join(d, "r%03d" % i))
    os.fsync(fd_dir)
dt = time.time() - t0
print("300 write+fsync+rename+dirfsync in %.1f s, %.1f ms each" % (dt, dt * 1000 / 300))
PY
(cd $T && find . -type f | sort | xargs sha256sum) > "$OUT/sums.txt"
log "$(wc -l < "$OUT/sums.txt") files summed"
sync
umount $M || fail "umount"
echo 0 > $P/mount
log "FTL unmounted (checkpoint)"
dmesg | grep -E "apple_ftl|sftl|apple-ppn" | grep -v "tree of" | tail -20 | tee -a "$OUT/log.txt"

echo 1 > $P/mount || fail "FTL remount"
dmesg | grep "apple_ftl: context" | tail -1 | tee -a "$OUT/log.txt"
$E2 -fn /dev/ftl0p2 > "$OUT/fsck-after.txt" 2>&1; log "e2fsck after: exit $?"
mount -t ext4 -o ro /dev/ftl0p2 $M || fail "ro mount"
(cd $T && sha256sum -c "$OUT/sums.txt" 2>&1 | grep -vc ": OK$") > /tmp/bad; log "files not OK after remount: $(cat /tmp/bad)"
umount $M
echo 0 > $P/mount
rmmod apple_ftl; rmmod apple_ppn
log "done"
