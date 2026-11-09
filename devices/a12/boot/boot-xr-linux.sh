#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IN="$ROOT/a12/boot/inputs"
LINUX_IMG4="${XR_LINUX_IMG4:-$IN/m1n1-linux.img4}"
LOGDIR="$ROOT/a12/boot/logs"
if [[ -n "${USBLITER8CTL:-}" ]]; then
  :
elif command -v usbliter8ctl >/dev/null 2>&1; then
  USBLITER8CTL="$(command -v usbliter8ctl)"
elif [[ -x "$ROOT/a13/usbliter8ctl" ]]; then
  USBLITER8CTL="$ROOT/a13/usbliter8ctl"
else
  USBLITER8CTL=/picotool/spironolactone/Linux/aarch64/usbliter8ctl
fi
[[ -x "$USBLITER8CTL" ]] || {
  echo "Missing usbliter8ctl; set USBLITER8CTL=/path/to/usbliter8ctl" >&2
  exit 1
}
mkdir -p "$LOGDIR"

for f in iBSS.linux.serial.raw DeviceTree.img4 RestoreTrustCache.img4; do
  [[ -s "$IN/$f" ]] || { echo "Missing $IN/$f" >&2; exit 1; }
done
[[ -s "$LINUX_IMG4" ]] || { echo "Missing $LINUX_IMG4" >&2; exit 1; }
lsusb | grep -q '05ac:1227' || { echo "XR is not in DFU" >&2; exit 1; }

if [[ -f "$LOGDIR/dcsd.pid" ]]; then
  kill "$(cat "$LOGDIR/dcsd.pid")" 2>/dev/null || true
  rm -f "$LOGDIR/dcsd.pid"
fi
if [[ -e /dev/ttyACM0 ]]; then
  LOG="$LOGDIR/xr-linux-$(date -u +%Y%m%d-%H%M%S).log"
  : > "$LOG"
  stty -F /dev/ttyACM0 115200 cs8 -cstopb -parenb -crtscts clocal raw -echo
  setsid -f sh -c 'echo $$ > "$1"; exec cat /dev/ttyACM0 >> "$2" 2>/dev/null' \
    sh "$LOGDIR/dcsd.pid" "$LOG"
  echo "$LOG" > "$LOGDIR/current.logpath"
  echo "DCSD log: $LOG"
else
  echo "No DCSD logger at /dev/ttyACM0; booting without a serial log"
fi

python3 "$USBLITER8CTL" boot "$IN/iBSS.linux.serial.raw" \
  >"$LOGDIR/usbliter8-last.log" 2>&1
for _ in $(seq 1 50); do
  if irecovery -q >/dev/null 2>&1; then
    sleep .5
    if irecovery -q >/dev/null 2>&1; then break; fi
  fi
  sleep .2
done
irecovery -q >/dev/null

irecovery -f "$IN/DeviceTree.img4"
irecovery -c devicetree
irecovery -f "$IN/RestoreTrustCache.img4"
irecovery -c firmware
irecovery -f "$LINUX_IMG4"
irecovery -c bootx
if [[ -f "$LOGDIR/dcsd.pid" ]]; then
  echo "Linux payload sent; logging continues as PID $(cat "$LOGDIR/dcsd.pid")"
else
  echo "Linux payload sent"
fi
