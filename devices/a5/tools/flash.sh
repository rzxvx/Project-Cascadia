#!/usr/bin/env bash
#
# Send the boot chain to a device in DFU:  iBSS -> iBEC -> bundle -> loader.
#
#   ./cascadia a5 flash                 use the freshly built iBEC
#   ./cascadia a5 flash --known-good    use the preserved August image instead
#   ./cascadia a5 flash --no-uart       skip the serial capture
#   ./cascadia a5 flash --no-link       leave this host's side of the network alone
#   ./cascadia a5 flash --kdfu          reach pwned DFU without a Pi Pico
#   ./cascadia a5 flash --skip-pwn      the device is already in pwned DFU
#   FLASH_OUT=DIR ./cascadia a5 flash   boot DIR's staging-bundle.bin + staging-loader.bin
#
# The order and the pauses are not decoration.  Our patched iBSS is started
# first; it accepts the unsigned iBEC; the iBEC's auto-go hook fires when the
# bundle upload ends and runs the loader, which is why the loader is sent last
# and why nothing here sends "go".
#
# Getting to "a pwned iBSS is running" has two routes.  checkm8 on A5 needs
# hardware that can drive USB with tighter timing than a general-purpose host
# manages -- a Raspberry Pi Pico (or an Arduino with a USB host shield) runs it,
# and the iPad, pwned, comes back to this computer in DFU; lib/appleusb.py then
# starts the iBSS on it.  kDFU is the other route: from a jailbroken iOS,
# kloader boots the iBSS directly (tools/kdfu.sh), no extra hardware at all.
# EverPwnage jailbreaks A5 on iOS 7-9.3.6 untethered, so the device comes up
# jailbroken every time and this stays a one-command step.  The USB side is
# lib/appleusb.py (pyusb): no irecovery, primepwn or Legacy iOS Kit.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"
USB=(python3 "$REPO/lib/appleusb.py")
FW="$ROOT/build/firmware"
LOG="${LOG:-$ROOT/uart.txt}"
UART_SECONDS="${UART_SECONDS:-}"   # the default depends on the route; see below

IBEC="$FW/iBEC.patched.autogo.dfu"
IBEC_WHICH="freshly built"
IBEC_PINNED=0          # --known-good and --ibec pin the image; kDFU must not
                       # then quietly swap it for another one
WANT_UART=1
WANT_LINK=1
PWN_MODE=pico

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
# switched off.  ./cascadia a5 firmware builds the iBEC both ways for this
# reason.
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

# pyusb (and the libusb under it) is the one thing the USB side needs.
python3 "$REPO/lib/appleusb.py" mode >/dev/null || fail "no pyusb or libusb (above): ./cascadia a5 setup"

# FLASH_OUT=DIR boots another bundle + loader pair (a test kernel built
# elsewhere); the pair must come from one build, as the loader knows where
# that kernel's DTB sits.
BOUT="${FLASH_OUT:-$ROOT/output}"
NEED=("$IBEC" "$BOUT/staging-bundle.bin" "$BOUT/staging-loader.bin")
# --skip-pwn means something already started an iBSS; the other two start
# ours.  `if`, not `[ ... ] && ...`: under set -e a failing test takes the
# script down, and here it fails in exactly the modes this is meant to support.
if [ "$PWN_MODE" != none ]; then NEED+=("$FW/iBSS.patched"); fi
for f in "${NEED[@]}"; do
    [ -f "$f" ] || fail "missing $f
    ./cascadia a5 firmware   builds iBSS/iBEC
    ./cascadia a5 build      builds the bundle and the loader"
done

case "$PWN_MODE" in
    pico)     echo "==> pwn:    checkm8 from the Pi Pico, then $FW/iBSS.patched" ;;
    kdfu)     echo "==> pwn:    kDFU from the jailbroken iOS (tools/kdfu.sh, no extra hardware)" ;;
    none)     echo "==> pwn:    skipped -- assuming the device is already in pwned DFU" ;;
esac
echo "==> iBEC:   $IBEC  ($IBEC_WHICH)"
echo "==> bundle: $BOUT/staging-bundle.bin"

# --skip-pwn cannot know how the device reached pwned DFU, and the two routes
# want different images: checkm8 from a Pico leaves the GID key usable and takes
# the stock encrypted iBEC, while a kDFU done by hand -- kloader from a
# jailbroken iOS -- does not, and needs the plaintext one.  --kdfu picks the
# right image because it did the pwning itself; here we can only say so.
if [ "$PWN_MODE" = none ] && [ "$IBEC_PINNED" = 0 ]; then
    case "$IBEC" in
        *.plain.dfu) : ;;
        *) echo "    note: this is the ENCRYPTED iBEC, which is right after checkm8 (Pico)" >&2
           echo "    and wrong after a hand-made kDFU from iOS -- that one uploads to 100%" >&2
           echo "    and the device goes dark.  If you got here with kloader, re-run with" >&2
           echo "      --ibec $FW/iBEC.patched.autogo.plain.dfu" >&2 ;;
    esac
fi

# ---------------------------------------------------------------- UART -----
# Optional, and best-effort.  The device also writes its early boot log to the
# framebuffer, and the UART capture has a habit of truncating partway through a
# boot anyway, so a missing cable is a nuisance rather than a blocker.
CAP=""
SUDO_KEEP=""
# Without this the capture outlives the script: on a failure `set -e` takes the
# shell down mid-flash and the python sits on the serial port for the rest of
# UART_SECONDS.
cleanup() {
    if [ -n "$CAP" ]; then kill "$CAP" 2>/dev/null || true; fi
    if [ -n "$SUDO_KEEP" ]; then kill "$SUDO_KEEP" 2>/dev/null || true; fi
}
trap cleanup EXIT

# sudo for the link, asked NOW.  The address goes on this computer's end of
# the link while the iPad boots, and stage 1 gives up on the NFS root ~30 s
# after its USB comes up; a sudo password asked at that moment -- a cached
# one expires after five minutes, and a kDFU or the install before it takes
# longer -- was typed too late, and the iPad came up from RAM.  So the
# password comes first, and is kept fresh until the end.
if [ "$WANT_LINK" = 1 ] && [ "$(id -u)" != 0 ]; then
    echo "==> sudo, for this computer's address on the iPad's link (asked now, not mid-boot)"
    sudo -v || fail "the link needs sudo -- or --no-link, and ./cascadia a5 link yourself"
    ( while kill -0 $$ 2>/dev/null; do sudo -n -v 2>/dev/null || true; sleep 50; done ) &
    SUDO_KEEP=$!
fi

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
# No sudo: on macOS libusb reaches a device in DFU as the user, and on Linux
# appleusb.py says which udev rule gives it to you when it cannot.
echo "==> boot chain"
case "$PWN_MODE" in
    pico)
        echo "==> waiting for the iPad in DFU (pwned by the Pico, now on this computer)"
        "${USB[@]}" wait DFU 60 || fail "no device in DFU.  Plug the iPad (in DFU) into the Pico, wait
for its LED to say done, then plug it into this computer and run this again."
        "${USB[@]}" boot-pwned "$FW/iBSS.patched"
        sleep 2 ;;
    kdfu)
        # kdfu.sh returns once the iPad is back in DFU, with our iBSS running
        bash "$ROOT/tools/kdfu.sh" ;;
    none) : ;;
esac
echo "==> waiting for the iBSS (DFU)"
"${USB[@]}" wait DFU 15 || fail "the iBSS did not come up in DFU (the device is: $("${USB[@]}" mode))"
"${USB[@]}" send "$IBEC";                                  sleep 1

# An iBEC upload always "succeeds": it means the host finished sending, not
# that anything is still listening.  Ask the device what it is before throwing
# 21 MB at it, so an iBEC that never came up is reported as that, and not as a
# failed bundle upload three lines later.  A warning rather than an error --
# the upload below is the real test, and this check is not worth breaking a
# working bench over.
echo "==> waiting for the iBEC to come up in Recovery"
back=0
"${USB[@]}" wait Recovery 10 2>/dev/null && back=1
if [ "$back" = 0 ]; then
    echo "    it did not.  The device is: $("${USB[@]}" mode | sed 's/^none$/not on the bus at all/')" >&2
    if [ "$PWN_MODE" != pico ]; then
        case "$IBEC" in
            *.plain.dfu)
                echo "    The image was already the unencrypted one, so it is not the KBAG." >&2
                echo "    The UART capture and the screen are where to look next." >&2 ;;
            *)
                echo "    Off a jailbroken iOS this is usually an ENCRYPTED iBEC: once iOS has" >&2
                echo "    booted the AES GID key is gone, the KBAG decrypts to nothing, and the" >&2
                echo "    iBSS jumps into garbage.  Use build/firmware/iBEC.patched.autogo.plain.dfu" >&2
                echo "    -- ./cascadia a5 firmware builds it, --kdfu picks it by default, and with" >&2
                echo "    --skip-pwn you have to pass it yourself: --ibec <that file>." >&2 ;;
        esac
    fi
fi

"${USB[@]}" send "$BOUT/staging-bundle.bin";        sleep 1
"${USB[@]}" send "$BOUT/staging-loader.bin"

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
    grep -E "AIC1-REARM|LATE-SMOKE|SOF-TIMER|AIC-TIMER|P105:" "$LOG" \
        || echo "  none captured -- the framebuffer has the early log too"
fi
