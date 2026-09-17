#!/usr/bin/env bash
#
# Send the boot chain to a device in DFU:  iBSS -> iBEC -> bundle -> loader.
#
#   ./cascadia flash                 use the freshly built iBEC
#   ./cascadia flash --known-good    use the preserved August image instead
#   ./cascadia flash --no-uart       skip the serial capture
#   ./cascadia flash --kdfu          reach pwned DFU without a Pi Pico
#   ./cascadia flash --skip-pwn      the device is already in pwned DFU
#
# The order and the pauses are not decoration.  primepwn runs checkm8 and leaves
# a pwned iBSS running; that iBSS accepts the unsigned iBEC; the iBEC's auto-go
# hook fires when the bundle upload ends and runs the loader, which is why the
# loader is sent last and why nothing here passes `irecovery -c go`.
#
# Getting to "a pwned iBSS is running" has two routes.  checkm8 on A5 needs
# hardware that can drive USB with tighter timing than a general-purpose host
# manages -- a Raspberry Pi Pico or an Arduino with a USB host shield -- which
# is a real barrier for someone who owns the iPad and nothing else.  kDFU is the
# other route: from a jailbroken iOS, kloader boots a patched iBSS directly, no
# extra hardware at all.  EverPwnage jailbreaks A5 on iOS 7-9.3.6 untethered, so
# the device comes up jailbroken every time and this stays a one-command step.
# Legacy iOS Kit implements both; this calls it rather than reimplementing it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIK="${LIK:-$HOME/Legacy-iOS-Kit}"
FW="$ROOT/build/firmware"
LOG="${LOG:-$ROOT/uart.txt}"
UART_SECONDS="${UART_SECONDS:-240}"

IBEC="$FW/iBEC.patched.autogo.dfu"
IBEC_WHICH="freshly built"
WANT_UART=1
PWN_MODE=primepwn

while [ $# -gt 0 ]; do
    case "$1" in
        --known-good)
            IBEC="${KNOWN_GOOD_IBEC:-$ROOT/ibootfiles/iBEC.patched.autogo.lk.dfu}"
            IBEC_WHICH="preserved known-good"; shift ;;
        --no-uart) WANT_UART=0; shift ;;
        --kdfu)     PWN_MODE=kdfu; shift ;;
        --skip-pwn) PWN_MODE=none; shift ;;
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
LIKMSG="    git clone https://github.com/LukeZGD/Legacy-iOS-Kit.git ~/Legacy-iOS-Kit
Or set LIK=/path/to/it."

# Only demand what this run will actually use.  With irecovery installed
# system-wide -- which it is on most Linux boxes, from libimobiledevice --
# --skip-pwn needs nothing from Legacy iOS Kit at all.
PRIMEPWN="$LBIN/primepwn"
IRECOVERY="$(command -v irecovery || echo "$LBIN/irecovery")"
[ -x "$IRECOVERY" ] || fail "no irecovery: not on PATH and not at $LBIN/irecovery
$LIKMSG"
case "$PWN_MODE" in
    primepwn) [ -x "$PRIMEPWN" ] || fail "no primepwn at $PRIMEPWN
It runs checkm8 on A5 and needs a Pi Pico or an Arduino with a USB host shield.
Without that hardware use --kdfu, which boots a patched iBSS from a jailbroken
iOS instead and needs nothing extra.
$LIKMSG" ;;
    kdfu)     [ -x "$LIK/restore.sh" ] || fail "no $LIK/restore.sh
kDFU is Legacy iOS Kit's; this calls it rather than reimplementing it.
$LIKMSG" ;;
esac

NEED=("$IBEC" "$ROOT/output/staging-bundle.bin" "$ROOT/output/staging-loader.bin")
# Only the primepwn route uploads our own iBSS.  kDFU boots Legacy iOS Kit's
# pwned iBSS from inside iOS, and --skip-pwn means something already did.
# `if`, not `[ ... ] && ...`: under set -e a failing test takes the script
# down, and here it fails in exactly the modes this is meant to support.
if [ "$PWN_MODE" = primepwn ]; then NEED+=("$FW/iBSS.patched"); fi
for f in "${NEED[@]}"; do
    [ -f "$f" ] || fail "missing $f
    ./cascadia firmware   builds iBSS/iBEC
    ./cascadia build      builds the bundle and the loader"
done

case "$PWN_MODE" in
    primepwn) echo "==> pwn:    checkm8 via primepwn, with $FW/iBSS.patched" ;;
    kdfu)     echo "==> pwn:    kDFU via $LIK/restore.sh --kdfu (no extra hardware)" ;;
    none)     echo "==> pwn:    skipped -- assuming the device is already in pwned DFU" ;;
esac
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
case "$PWN_MODE" in
    primepwn)
        sudo "$PRIMEPWN" "$FW/iBSS.patched"; sleep 1 ;;
    kdfu)
        # restore.sh resolves its own resources relatively, so it has to run
        # from its own directory.  It is interactive by design -- it will say
        # what to do with the device -- so it is not piped or backgrounded.
        #
        # Note it sends ITS pwned iBSS, not ours.  Ours differs only in the
        # boot-args iBoot32Patcher wrote in; what matters is the signature
        # patch, which both have, and iBEC sets its own boot-args anyway.
        ( cd "$LIK" && ./restore.sh --kdfu )
        echo "==> kDFU done; waiting for the device to come back"
        sleep 3 ;;
    none) : ;;
esac
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
