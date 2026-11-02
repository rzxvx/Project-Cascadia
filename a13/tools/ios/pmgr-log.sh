#!/bin/sh
# iOS's own record of a GPU power-up (gpu.md, G4): turns on ApplePMGR's debug
# log in the running kernel, lets the GPU power off and on, saves the kernel
# log.  The phone on iOS 16.4 + Dopamine with kmem installed (build.sh).
#   pmgr-log.sh [SECS] [MASK]     on the Mac; SSH="ssh iphone11-ios" by default
#   pmgr-log.sh off               clears it after a debug-ibss.py boot
#
# pmgr-debug (boot-arg) is parsed only when PE_i_can_has_debugger() is true,
# which a production phone is not, so the value goes straight into the field
# it would have filled: ApplePMGR +0xe0 (parse at ApplePMGR fn 0xfffffff009194540,
# 20E247).  Bits, from the format strings each one gates:
#   1   PWR_*_CFG0 / *_PS: MANUAL_PS, AUTO_PM_EN
#   2   autoClockGating / autoPowerGating / reset, per device
#   3   "%s: %u -> %u": every device power state change
#   11  PTD-SOC-DEV-PS-REQ / -ACK / -DVFS: device requests sent to the PMP
#   15  through IOLog (dmesg) rather than kprintf (serial only)
# During SECS: lock the phone, wait ~5 s, unlock and scroll, so the GPU
# powers off and back on.  The kernel's message buffer holds only ~230 lines
# and SPI3 fills it fast, so dmesg is read every 0.2 s and merged here.
# The phone has no awk; parsing happens on the Mac.
set -eu
SECS=${1:-30}
MASK=${2:-0x880e}
SSH=${SSH:-ssh iphone11-ios}
KMEM=/var/jb/usr/local/bin/kmem
OUT=$(dirname "$0")/../../build/gpu/pmgr-log
mkdir -p "$OUT"
log=$OUT/pmgr-log-$(date +%H%M%S).txt
obj=$($SSH $KMEM obj AppleT8030PMGR | awk '{ print $2; exit }')
[ -n "$obj" ] || { echo "no AppleT8030PMGR" >&2; exit 1; }
# shell arithmetic is signed 64-bit; kernel addresses are not
field=$(python3 -c "print(hex($obj + 0xe0))")
# the 32-bit field, little-endian, from the dump's first four bytes
old=$($SSH $KMEM kr "$field" 4 | awk '{ print "0x" $5 $4 $3 $2; exit }')
case $old in 0x[0-9a-f]*) ;; *) echo "could not read $field" >&2; exit 1 ;; esac
# after a boot with pmgr-debug on the command line (debug-ibss.py): off
if [ "$SECS" = off ]; then
	echo "pmgr-debug at $field was $old"
	$SSH $KMEM kw "$field" 32 0
	exit
fi
echo "AppleT8030PMGR $obj, pmgr-debug at $field = $old; ${SECS} s with $MASK"
$SSH sh -s "$KMEM" "$field" "$old" "$MASK" "$SECS" > "$log.raw" <<'EOF'
KMEM=$1 f=$2 old=$3 MASK=$4 SECS=$5
trap '$KMEM kw "$f" 32 "$old" >&2' EXIT INT TERM HUP
$KMEM kw "$f" 32 "$MASK" >&2
n=$((SECS * 5))
while [ "$n" -gt 0 ]; do
	/var/jb/sbin/dmesg
	sleep 0.2
	n=$((n - 1))
done
EOF
# every poll repeats the whole buffer: keep each line once, in time order
grep '^\[' "$log.raw" | sort -u | sort -s -n -t '[' -k2,2 > "$log"
rm -f "$log.raw"
echo "$log: $(grep -c 'ApplePMGR' "$log" || true) ApplePMGR lines"
grep -iE 'gfx|sgx|gpu|afr|afnc|ptd' "$log" | head -60 || true
