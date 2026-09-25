#!/usr/bin/env bash
#
# Touch needs two files from the iPad's own iOS, and this fetches both:
#
#   ./cascadia mtcal                       iPad booted into its jailbroken iOS, on USB
#   IOS_HOST=192.168.1.20 ./cascadia mtcal the same over Wi-Fi
#
#   build/keep/lib/firmware/mtcal.bin      this panel's calibration (tools/mtdump/mtcal.c)
#   build/keep/lib/firmware/P105.mtprops   the digitizer's firmware, from /usr/share/firmware/multitouch
#
# Neither can ship with the repository: the firmware is Apple's, and the
# calibration is per panel -- iBoot copies it out of syscfg (MtCl) at every
# boot, so there is no other place to read it than a running iOS.  Without them
# stage 2 does not start touch.  ./cascadia build carries build/keep/ into the
# image, so: this, then build, then flash.  The jailbroken iOS with OpenSSH is
# the one ./cascadia flash --kdfu starts from anyway.
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
MTPROPS_IOS=/usr/share/firmware/multitouch/P105.mtprops

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
[ "$model" = "iPad2,5" ] || fail "this is a $model; the calibration and firmware here are for iPad2,5 (p105ap)"
echo "    $model"

# rm before writing: iOS caches a binary's signature by vnode, and a file
# rewritten in place is killed on sight ("Killed: 9").
ios 'rm -f /tmp/mtcal && cat > /tmp/mtcal && chmod 755 /tmp/mtcal' < "$HELPER"
ios /tmp/mtcal > "$WORK/mtcal.bin" || fail "mtcal failed on the iPad (its message is above)"
ios rm -f /tmp/mtcal
ios cat "$MTPROPS_IOS" > "$WORK/P105.mtprops" || fail "no $MTPROPS_IOS on the iPad"

# The calibration is a blob with no header to check, so only what would make
# it certainly wrong is refused: nothing, too much, or all zeros -- the last
# being what a panel iBoot found no MtCl for would carry.
n=$(wc -c < "$WORK/mtcal.bin" | tr -d ' ')
[ "$n" -gt 0 ] && [ "$n" -le 65536 ] || fail "calibration is $n bytes -- not believable"
od -An -tx1 "$WORK/mtcal.bin" | tr -d ' \n' | grep -q '[1-9a-f]' || fail "calibration is all zeros"
case "$(head -c 6 "$WORK/P105.mtprops")" in
    '<?xml'*|bplist) : ;;
    *) fail "$MTPROPS_IOS does not look like a plist" ;;
esac

mkdir -p "$KEEP"
for f in mtcal.bin P105.mtprops; do
    if [ -f "$KEEP/$f" ] && cmp -s "$WORK/$f" "$KEEP/$f"; then
        echo "==> $f: unchanged ($(wc -c < "$KEEP/$f" | tr -d ' ') bytes)"
        continue
    fi
    # Another iPad, or a replaced panel: the old one is kept, not lost -- and
    # not in build/keep/, all of which goes into the image.
    if [ -f "$KEEP/$f" ]; then
        mkdir -p "$ROOT/build/prev"
        cp "$KEEP/$f" "$ROOT/build/prev/$f"
        echo "    (the previous $f is kept as build/prev/$f)"
    fi
    cp "$WORK/$f" "$KEEP/$f"
    echo "==> $f: $(wc -c < "$KEEP/$f" | tr -d ' ') bytes -> build/keep/lib/firmware/"
done
echo
echo "    Into the image and onto the iPad:  ./cascadia build && ./cascadia flash --kdfu"
