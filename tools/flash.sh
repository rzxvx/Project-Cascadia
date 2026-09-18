#!/usr/bin/env bash
#
# Send the boot chain to a device in DFU:  iBSS -> iBEC -> bundle -> loader.
#
#   ./cascadia flash                 use the freshly built iBEC
#   ./cascadia flash --known-good    use the preserved August image instead
#   ./cascadia flash --no-uart       skip the serial capture
#   ./cascadia flash --no-link       leave this host's side of the network alone
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
UART_SECONDS="${UART_SECONDS:-}"   # the default depends on the route; see below

IBEC="$FW/iBEC.patched.autogo.dfu"
IBEC_WHICH="freshly built"
IBEC_PINNED=0          # --known-good and --ibec pin the image; kDFU must not
                       # then quietly swap it for another one
WANT_UART=1
WANT_LINK=1
PWN_MODE=primepwn

while [ $# -gt 0 ]; do
    case "$1" in
        --known-good)
            IBEC="${KNOWN_GOOD_IBEC:-$ROOT/ibootfiles/iBEC.patched.autogo.lk.dfu}"
            IBEC_WHICH="preserved known-good"; IBEC_PINNED=1; shift ;;
        --no-uart) WANT_UART=0; shift ;;
        --no-link) WANT_LINK=0; shift ;;
        --kdfu)     PWN_MODE=kdfu; shift ;;
        --skip-pwn) PWN_MODE=none; shift ;;
        --ibec) IBEC="$2"; IBEC_WHICH="explicit"; IBEC_PINNED=1; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

fail() { echo "error: $*" >&2; exit 1; }

# kDFU cannot be handed an encrypted image.  Once iOS has booted, the AES GID
# key is no longer usable, so the KBAG of a stock-layout img3 decrypts to
# nothing and the pwned iBSS jumps into garbage.  Nothing says so at the time:
# irecovery reports 100%, and then the device drops off the bus and looks
# switched off.  Legacy iOS Kit's own pwned iBSS is plaintext for this reason,
# and ./cascadia firmware builds the same iBEC both ways for this reason.
# The capture starts before the boot chain does, and kDFU's first half is a
# person: a Trust prompt, a root password, an unplug and replug.  240 s can be
# spent before the kernel even starts, so that route gets a longer window.
if [ -z "$UART_SECONDS" ]; then
    case "$PWN_MODE" in
        kdfu) UART_SECONDS=600 ;;
        *)    UART_SECONDS=240 ;;
    esac
fi

if [ "$PWN_MODE" = kdfu ] && [ "$IBEC_PINNED" = 0 ]; then
    IBEC="$FW/iBEC.patched.autogo.plain.dfu"
    IBEC_WHICH="freshly built, unencrypted for kDFU"
fi

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
# Without this the capture outlives the script: on a failure `set -e` takes the
# shell down mid-flash and the python sits on the serial port for the rest of
# UART_SECONDS.
cleanup() { if [ -n "$CAP" ]; then kill "$CAP" 2>/dev/null || true; fi; }
trap cleanup EXIT

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
end = time.time() + secs
# Write through rather than buffering until the end.  This process is killed
# whenever the flash stops early, and a log that exists only in memory dies
# with it -- which is precisely the run whose log is worth having.
with open(log, "wb") as f:
    while time.time() < end:
        c = s.read(65536)
        if c:
            f.write(c); f.flush()
            sys.stdout.write(c.decode("utf-8", "replace")); sys.stdout.flush()
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
        # restore.sh returning is not the device being in kDFU.  On its first
        # run on Linux it installs its own dependencies instead, says "run the
        # script again", and exits -- with success -- having never touched the
        # device, and this used to carry on into an iBEC upload that could only
        # fail with irecovery's "Unable to connect to device".  So ask the
        # device, the same way restore.sh itself checks for kDFU.
        echo "==> waiting for the device in DFU (kDFU)"
        indfu=0
        for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
            if sudo "$IRECOVERY" -q 2>/dev/null | grep -q "MODE: DFU"; then indfu=1; break; fi
            sleep 1
        done
        if [ "$indfu" = 0 ]; then
            fail "Legacy iOS Kit returned, but no device is in DFU mode -- kloader never ran.
Scroll up to its output.  The usual cause is a first run on Linux: it installs
its own dependencies instead, says \"run the script again\" and exits without
touching the device.  Run it once on its own until it reaches its menu, then
flash again:
    cd $LIK && ./restore.sh"
        fi
        echo "==> device is in kDFU" ;;
    none) : ;;
esac
sudo "$IRECOVERY" -f "$IBEC";                               sleep 1

# An iBEC upload always "succeeds": it means the host finished sending, not
# that anything is still listening.  Ask the device what it is before throwing
# 21 MB at it, so an iBEC that never came up is reported as that, and not as a
# failed bundle upload three lines later.  A warning rather than an error --
# the upload below is the real test, and this check is not worth breaking a
# working bench over.
echo "==> waiting for the iBEC to come up in Recovery"
back=0
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if sudo "$IRECOVERY" -q 2>/dev/null | grep -q "MODE: Recovery"; then back=1; break; fi
    sleep 1
done
if [ "$back" = 0 ]; then
    echo "    it did not.  The device is: $(sudo "$IRECOVERY" -q 2>/dev/null | grep -w MODE || echo "not on the bus at all")" >&2
    if [ "$PWN_MODE" = kdfu ]; then
        case "$IBEC" in
            *.plain.dfu)
                echo "    The image was already the unencrypted one, so it is not the KBAG." >&2
                echo "    The UART capture and the screen are where to look next." >&2 ;;
            *)
                echo "    On the kDFU route this is usually an ENCRYPTED iBEC: after iOS has" >&2
                echo "    booted the AES GID key is gone, the KBAG decrypts to nothing, and the" >&2
                echo "    iBSS jumps into garbage.  Use build/firmware/iBEC.patched.autogo.plain.dfu" >&2
                echo "    -- ./cascadia firmware builds it, and --kdfu picks it by default." >&2 ;;
        esac
    fi
fi

sudo "$IRECOVERY" -f "$ROOT/output/staging-bundle.bin";     sleep 1
sudo "$IRECOVERY" -f "$ROOT/output/staging-loader.bin"

# This host's end of the link, now, while the device boots.  Until the gadget
# interface here has 10.55.0.1 nothing reaches the device over the network --
# ssh does not fail, it hangs -- and stage 1's NFS mount gives up after about
# two minutes and stays on the RAM root.  Not fatal: the ACM console needs no
# address, and the verdicts below still come from the capture.
if [ "$WANT_LINK" = 1 ]; then
    bash "$ROOT/tools/host-link.sh" \
        || echo "==> link did not come up; the ACM console still works" >&2
fi

[ -n "$CAP" ] && wait "$CAP" || true

if [ -n "$CAP" ] && [ -f "$LOG" ]; then
    echo
    echo "==> verdict lines"
    grep -E "AIC1-REARM|LATE-SMOKE|SOF-TIMER|P105:" "$LOG" \
        || echo "  none captured -- the framebuffer has the early log too"
fi
