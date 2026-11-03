#!/bin/sh
# Records the phone's serial console (DCSD cable) to a file, for a boot with
# serial=3 in the boot-args (debug-ibss.py).  Ctrl-C to stop.
#   serial-log.sh [DEVICE]
set -eu
DEV=${1:-$(ls /dev/cu.usbserial-* | head -1)}
OUT=$(dirname "$0")/../../build/gpu/pmgr-log
mkdir -p "$OUT"
log=$OUT/serial-$(date +%H%M%S).txt
echo "$DEV -> $log"
# stty on the same open descriptor cat reads, so the settings hold; the
# port ends when the cable is unplugged (Pi Pico for the pwn), so reopen
while :; do
	[ -e "$DEV" ] && { stty 115200 raw -echo; cat; } < "$DEV" 2>/dev/null || true
	sleep 1
done | tee "$log"
