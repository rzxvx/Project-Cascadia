#!/bin/sh
# tools/smp-stress.sh -- two-core smoke test, run on the device.  Parallel
# hashing of one blob (every result must match), a fork/pipe storm (migration,
# IPIs), per-CPU time before and after.  RUNS= hashes per worker, FORKS= forks.
dd if=/dev/urandom of=/tmp/blob bs=1M count=8 2>/dev/null
ref=$(sha256sum /tmp/blob | cut -d' ' -f1)
grep -E "^cpu[01] " /proc/stat
t0=$(date +%s)
worker() {
	n=0; bad=0
	while [ $n -lt ${RUNS:-20} ]; do
		h=$(sha256sum /tmp/blob | cut -d' ' -f1)
		[ "$h" = "$ref" ] || bad=$((bad + 1))
		n=$((n + 1))
	done
	echo "worker $1: $n runs, $bad bad"
}
worker a & worker b & worker c &
( i=0; while [ $i -lt ${FORKS:-400} ]; do echo $i | cat > /dev/null; i=$((i + 1)); done; echo "fork storm: $i done" ) &
wait
echo "took $(( $(date +%s) - t0 )) s"
grep -E "^cpu[01] " /proc/stat
grep . /sys/module/apple_smp/parameters/*
grep -E "IPI[23]" /proc/interrupts
uptime
