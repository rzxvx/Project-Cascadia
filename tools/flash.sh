#!/usr/bin/env bash
#
# Send the boot chain to a device in DFU:  iBSS -> iBEC -> bundle -> loader.
#
#   ./cascadia flash                 use the freshly built iBEC
#   ./cascadia flash --known-good    use the preserved August image instead
#   ./cascadia flash --no-uart       skip the serial capture
#
# The order and the pauses are not decoration.  primepwn runs checkm8 and leaves
# a pwned iBSS running; that iBSS accepts the unsigned iBEC; the iBEC's auto-go
# hook fires when the bundle upload ends and runs the loader, which is why the
# loader is sent last and why nothing here passes `irecovery -c go`.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIK="${LIK:-$HOME/Legacy-iOS-Kit}"
FW="$ROOT/build/firmware"
LOG="${LOG:-$ROOT/uart.txt}"
UART_SECONDS="${UART_SECONDS:-240}"

IBEC="$FW/iBEC.patched.autogo.dfu"
IBEC_WHICH="freshly built"
WANT_UART=1

while [ $# -gt 0 ]; do
    case "$1" in
        --known-good)
            IBEC="${KNOWN_GOOD_IBEC:-$ROOT/ibootfiles/iBEC.patched.autogo.lk.dfu}"
            IBEC_WHICH="preserved known-good"; shift ;;
        --no-uart) WANT_UART=0; shift ;;
        --ibec) IBEC="$2"; IBEC_WHICH="explicit"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

fail() { echo "error: $*" >&2; exit 1; }

# primepwn and irecovery come from Legacy iOS Kit rather than being vendored.
# The path used to be /Users/<someone>/Legacy-iOS-Kit/bin/macos/arm64/primepwn,
# which is why nobody else could run this.
case "$(uname -s)" in
    Darwin) LOS=macos ;;
    Linux)  LOS=linux ;;
    *) fail "unsupported host: $(uname -s)" ;;
esac
case "$(uname -m)" in
    arm64|aarch64) LARCH=arm64 ;;
    *) LARCH=x86_64 ;;
esac
LBIN="$LIK/bin/$LOS/$LARCH"
[ -d "$LBIN" ] || fail "no Legacy iOS Kit binaries at $LBIN
    git clone https://github.com/LukeZGD/Legacy-iOS-Kit.git ~/Legacy-iOS-Kit
Or set LIK=/path/to/it."

PRIMEPWN="$LBIN/primepwn"
IRECOVERY="$(command -v irecovery || echo "$LBIN/irecovery")"
[ -x "$PRIMEPWN" ]  || fail "no primepwn at $PRIMEPWN"
[ -x "$IRECOVERY" ] || fail "no irecovery at $IRECOVERY"

for f in "$FW/iBSS.patched" "$IBEC" "$ROOT/output/staging-bundle.bin" "$ROOT/output/staging-loader.bin"; do
    [ -f "$f" ] || fail "missing $f
    ./cascadia firmware   builds iBSS/iBEC
    ./cascadia build      builds the bundle and the loader"
done

echo "==> iBSS:   $FW/iBSS.patched"
echo "==> iBEC:   $IBEC  ($IBEC_WHICH)"
echo "==> bundle: $(basename "$ROOT/output/staging-bundle.bin")"

# ---------------------------------------------------------------- UART -----
# Optional, and best-effort.  The device also writes its early boot log to the
# framebuffer, and the UART capture has a habit of truncating partway through a
# boot anyway, so a missing cable is a nuisance rather than a blocker.
CAP=""
if [ "$WANT_UART" = 1 ]; then
    PORT="${UART_PORT:-}"
    if [ -z "$PORT" ]; then
        for p in /dev/cu.usbserial-* /dev/ttyUSB* /dev/cu.usbmodem*; do
            [ -e "$p" ] && { PORT="$p"; break; }
        done
    fi
    if [ -z "$PORT" ]; then
        echo "==> no serial adapter found; skipping the capture (--no-uart to silence)"
    elif ! python3 -c 'import serial' 2>/dev/null; then
        echo "==> python3 has no pyserial; skipping the capture (pip install pyserial)"
    else
        echo "==> UART $PORT -> $LOG (${UART_SECONDS}s)"
        python3 - "$PORT" "$LOG" "$UART_SECONDS" <<'PY' &
import serial, sys, time
port, log, secs = sys.argv[1], sys.argv[2], int(sys.argv[3])
s = serial.Serial(port, 115200, timeout=0.2)
end, buf = time.time() + secs, b""
while time.time() < end:
    c = s.read(65536)
    if c:
        buf += c
        sys.stdout.write(c.decode("utf-8", "replace")); sys.stdout.flush()
open(log, "wb").write(buf)
s.close()
PY
        CAP=$!
        sleep 2
    fi
fi

# --------------------------------------------------------------- upload ----
# sudo: raw USB to a device in DFU needs it on macOS, and on Linux without udev
# rules granting the device to your user.
echo "==> boot chain"
sudo "$PRIMEPWN" "$FW/iBSS.patched";                        sleep 1
sudo "$IRECOVERY" -f "$IBEC";                               sleep 1
sudo "$IRECOVERY" -f "$ROOT/output/staging-bundle.bin";     sleep 1
sudo "$IRECOVERY" -f "$ROOT/output/staging-loader.bin"

[ -n "$CAP" ] && wait "$CAP" || true

if [ -n "$CAP" ] && [ -f "$LOG" ]; then
    echo
    echo "==> verdict lines"
    grep -E "AIC1-REARM|LATE-SMOKE|SOF-TIMER|P105:" "$LOG" \
        || echo "  none captured -- the framebuffer has the early log too"
fi
