#!/bin/sh
# Ladder rung 10 (spec 8.2): sustained speed against spec 7.5 and long read
# passes, on the NAND root (verify every 16).  Writes only under /rung8-test/r10.
set -u
T=/rung8-test/r10
L=/rung8-test/r10.log
log() { echo "$(date +%T) $*" | tee -a $L; }
mb_s() { awk -v b="$1" -v s="$2" 'BEGIN{ if (s < 0.001) s = 0.001; printf "%.1f", b / 1048576 / s }'; }
now() { cat /proc/uptime | cut -d" " -f1; }
rm -rf $T; mkdir -p $T
log "verify_every=$(cat /sys/module/apple_ftl/parameters/verify_every)"

# Sequential write, 512 MiB, data on the media when dd returns.
s=$(now); dd if=/dev/zero of=$T/seq.bin bs=1M count=512 conv=fsync 2>/dev/null; e=$(now)
log "seq write 512 MiB (zeros, fsync): $(awk -v a=$s -v b=$e 'BEGIN{print b-a}') s = $(mb_s 536870912 $(awk -v a=$s -v b=$e 'BEGIN{print b-a}')) MiB/s"
dd if=/dev/urandom of=/tmp/rnd bs=1M count=64 2>/dev/null
s=$(now); for i in 1 2 3 4; do cat /tmp/rnd; done > $T/rnd.bin; sync; e=$(now)
log "seq write 256 MiB (random, sync): $(mb_s 268435456 $(awk -v a=$s -v b=$e 'BEGIN{print b-a}')) MiB/s"
rm -f /tmp/rnd

# Sequential read, cold cache.
sync; echo 3 > /proc/sys/vm/drop_caches
s=$(now); dd if=$T/seq.bin of=/dev/null bs=1M 2>/dev/null; e=$(now)
log "seq read 512 MiB (file, cold): $(mb_s 536870912 $(awk -v a=$s -v b=$e 'BEGIN{print b-a}')) MiB/s"

# Synced 4 KiB writes.
python3 - $T <<'PY' | tee -a $L
import os, sys, time
d = sys.argv[1]
lat = []
fd = os.open(os.path.join(d, "fsync.bin"), os.O_WRONLY | os.O_CREAT, 0o644)
for i in range(300):
    t = time.time()
    os.pwrite(fd, os.urandom(4096), (i % 64) * 4096)
    os.fsync(fd)
    lat.append((time.time() - t) * 1000)
os.close(fd)
lat.sort()
print("fsync 4 KiB x300: mean %.1f ms, median %.1f, p95 %.1f, max %.1f" %
      (sum(lat) / len(lat), lat[150], lat[285], lat[-1]))
PY

# Long read passes: the whole FTL device twice, cold.
for pass in 1 2; do
	sync; echo 3 > /proc/sys/vm/drop_caches
	s=$(now); dd if=/dev/ftl0 of=/dev/null bs=1M 2>/tmp/dd.err; rc=$?; e=$(now)
	log "read pass $pass of ftl0 (16 GB): exit $rc, $(awk -v a=$s -v b=$e 'BEGIN{print b-a}') s, $(tail -1 /tmp/dd.err)"
done
dmesg | grep -E "apple_ftl: (read|write)|sftl: ERR|reclaim|committed" | tail -10 | tee -a $L
rm -f $T/seq.bin $T/rnd.bin
log "rung 10 done"
