#!/bin/sh
# Which PMU rail follows the GPU (gpu.md, G4): on iOS 16.4 + Dopamine,
# samples Fender's state (+0x1000: 0 off, 1 on), the PMGR's voltage
# manager VOLMAN (0x23b0a0000: +0x4..+0x1c channel enables, +0x20 status,
# +0x24..+0x3c config; AppleT8030PMGR fns 0xfffffff009463e6c,
# 0xfffffff00946d804 read and write these, so reading them is safe) and,
# in turn, one PMU regulator block (8 bytes: +0 on, +1 voltage code).
#   rails-gpu.sh [SECS]     on the Mac; SSH="ssh iphone11-ios" by default
#
# One SPMI read per kmem run, 0.3 s apart: kmem's spmir drives the spmi0
# controller behind the kernel's back, and a tight loop of them with the
# screen on took iOS down (gpuvwatch, 2026-11-02). During SECS: screen off
# for the first third, then unlock and scroll / play something animated.
set -eu
SECS=${1:-60}
SSH=${SSH:-ssh iphone11-ios}
OUT=$(dirname "$0")/../../build/gpu/pmgr-diff
mkdir -p "$OUT"
log=$OUT/ios-rails-gpu-$(date +%H%M%S).txt
$SSH sh -s "$SECS" > "$log" <<'EOF'
SECS=$1
K=/var/jb/usr/local/bin/kmem
# bucks 0x4000-0x4500, 0x4b00; LDO-like blocks 0x4600-0x4a00, 0x4c00
set -- 0x4000 0x4100 0x4200 0x4300 0x4400 0x4500 0x4b00 0x4600 0x4700 0x4800 0x4900 0x4a00 0x4c00
n=$((SECS * 10 / 3))
i=0
while [ "$i" -lt "$n" ]; do
	for b in "$@"; do
		[ "$i" -lt "$n" ] || break
		f=$($K mmio 0x204d01000 4 2>&1 | head -1)
		v=$($K mmio 0x23b0a0000 0x40 2>&1 | tr '\n' ' ')
		r=$($K spmir 0xf "$b" 8 2>&1 | head -1)
		echo "$(date +%s) $i fender[$f] volman[$v] pmu[$r]"
		sleep 0.3
		i=$((i + 1))
	done
done
EOF
echo "$log"
