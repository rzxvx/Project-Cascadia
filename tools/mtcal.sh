#!/usr/bin/env bash
#
# This iPad's own touch calibration, from its own iOS.  Optional.
#
#   ./cascadia mtcal                       iPad booted into its jailbroken iOS, on USB
#   IOS_HOST=192.168.1.20 ./cascadia mtcal the same over Wi-Fi
#
# The calibration is per panel: iBoot copies it out of syscfg (MtCl) at every
# boot, and a running iOS is the only place to read it (tools/mtdump/mtcal.c).
# The image carries a default one, initramfs/lib/firmware/mtcal.bin, taken from
# the iPad this port was brought up on; this replaces it with the panel in
# hand, in build/keep/lib/firmware/mtcal.bin, which ./cascadia build lays over
# the default.  So: this, then build, then flash.  The jailbroken iOS with
# OpenSSH is the one ./cascadia flash --kdfu starts from anyway.  The
# digitizer's firmware is not fetched here: ./cascadia firmware takes it out
# of the IPSW.
#
# Over USB this goes through usbmuxd and iproxy: on the Mac usbmuxd is part of
# the system; on Linux it is the usbmuxd package (iproxy: libusbmuxd on Arch,
# libusbmuxd-tools on Debian/Ubuntu, libusbmuxd-utils on Fedora), or Legacy
# iOS Kit's copies.  One ssh connection is shared by every step, so iOS's root
# password is asked for once -- "alpine" unless it was changed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIK="${LIK:-$HOME/Legacy-iOS-Kit}"
KEEP="$ROOT/build/keep/lib/firmware"
HELPER="$ROOT/tools/mtdump/prebuilt/mtcal"

fail() { echo "error: $*" >&2; exit 1; }

[ -f "$HELPER" ] || fail "no $HELPER -- it is in the repository; a partial checkout?"

# /tmp and not $TMPDIR: the ssh control socket's path has to fit in a
# sockaddr_un, and macOS's TMPDIR alone takes half of it.
WORK=$(mktemp -d /tmp/cascadia-mtcal.XXXXXX)
IPROXY_PID=""
CM="$WORK/cm"
cleanup() {
    ssh -o ControlPath="$CM" -O exit x 2>/dev/null || true
    if [ -n "$IPROXY_PID" ]; then kill "$IPROXY_PID" 2>/dev/null || true; fi
    rm -rf "$WORK"
}
trap cleanup EXIT

if [ -n "${IOS_HOST:-}" ]; then
    HOST="$IOS_HOST"; PORT="${IOS_PORT:-22}"
    echo "==> iOS at $HOST:$PORT"
else
    HOST=127.0.0.1; PORT="${IOS_PORT:-2222}"
    IPROXY="$(command -v iproxy || true)"
    if [ -z "$IPROXY" ]; then
        c=""
        case "$(uname -s)/$(uname -m)" in
            Darwin/*)                     c="$LIK/bin/macos/iproxy" ;;
            Linux/aarch64|Linux/arm64)    c="$LIK/bin/linux/arm64/iproxy" ;;
            Linux/*)                      c="$LIK/bin/linux/x86_64/iproxy" ;;
        esac
        if [ -x "$c" ]; then IPROXY="$c"; fi
    fi
    [ -n "$IPROXY" ] || fail "no iproxy: not on PATH and not in Legacy iOS Kit ($LIK).
  Arch: sudo pacman -S usbmuxd libusbmuxd   Debian/Ubuntu: sudo apt install usbmuxd libusbmuxd-tools
  Fedora: sudo dnf install usbmuxd libusbmuxd-utils        Or over Wi-Fi: IOS_HOST=<the iPad's IP>"
    if [ "$(uname -s)" = Linux ] && ! pgrep -x usbmuxd >/dev/null 2>&1; then
        fail "usbmuxd is not running.  It starts by itself when an iOS device is plugged in
once the usbmuxd package is installed; or: sudo systemctl start usbmuxd"
    fi
    echo "==> iOS over USB: $IPROXY $PORT 22"
    "$IPROXY" "$PORT" 22 >"$WORK/iproxy.log" 2>&1 &
    IPROXY_PID=$!
    sleep 1
    kill -0 "$IPROXY_PID" 2>/dev/null || fail "iproxy exited: $(cat "$WORK/iproxy.log")"
fi

# The same settings Legacy iOS Kit uses for that sshd: ssh-rsa host keys only.
ios() {
    ssh -F "$ROOT/tools/mtdump/ssh_config" -o ControlMaster=auto -o ControlPath="$CM" \
        -o ControlPersist=120 -o ConnectTimeout=10 -p "$PORT" "root@$HOST" "$@"
}

echo "==> connecting (iOS root password, 'alpine' unless changed)"
model=$(ios uname -m) || fail "no ssh to iOS on $HOST:$PORT -- is the iPad in iOS, unlocked, with OpenSSH installed?"
[ "$model" = "iPad2,5" ] || fail "this is a $model; this port is for iPad2,5 (p105ap)"
echo "    $model"

# rm before writing: iOS caches a binary's signature by vnode, and a file
# rewritten in place is killed on sight ("Killed: 9").
ios 'rm -f /tmp/mtcal && cat > /tmp/mtcal && chmod 755 /tmp/mtcal' < "$HELPER"
ios /tmp/mtcal > "$WORK/mtcal.bin" || fail "mtcal failed on the iPad (its message is above)"
ios rm -f /tmp/mtcal

# The calibration is a blob with no header to check, so only what would make
# it certainly wrong is refused: nothing, too much, or all zeros -- the last
# being what a panel iBoot found no MtCl for would carry.
n=$(wc -c < "$WORK/mtcal.bin" | tr -d ' ')
[ "$n" -gt 0 ] && [ "$n" -le 65536 ] || fail "calibration is $n bytes -- not believable"
od -An -tx1 "$WORK/mtcal.bin" | tr -d ' \n' | grep -q '[1-9a-f]' || fail "calibration is all zeros"

if cmp -s "$WORK/mtcal.bin" "$ROOT/initramfs/lib/firmware/mtcal.bin"; then
    echo "==> the same calibration as the default one: this is that iPad"
fi
mkdir -p "$KEEP"
if [ -f "$KEEP/mtcal.bin" ] && cmp -s "$WORK/mtcal.bin" "$KEEP/mtcal.bin"; then
    echo "==> mtcal.bin: unchanged ($n bytes)"
else
    # Another iPad, or a replaced panel: the old one is kept, not lost -- and
    # not in build/keep/, all of which goes into the image.
    if [ -f "$KEEP/mtcal.bin" ]; then
        mkdir -p "$ROOT/build/prev"
        cp "$KEEP/mtcal.bin" "$ROOT/build/prev/mtcal.bin"
        echo "    (the previous one is kept as build/prev/mtcal.bin)"
    fi
    cp "$WORK/mtcal.bin" "$KEEP/mtcal.bin"
    echo "==> mtcal.bin: $n bytes -> build/keep/lib/firmware/"
fi
echo
echo "    Into the image and onto the iPad:  ./cascadia build && ./cascadia flash --kdfu"
