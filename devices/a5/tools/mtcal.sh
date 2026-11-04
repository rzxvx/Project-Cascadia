#!/usr/bin/env bash
#
# This iPad's own touch calibration, from its own iOS.  Optional.
#
#   ./cascadia a5 mtcal                       iPad booted into its jailbroken iOS, on USB
#   IOS_HOST=192.168.1.20 ./cascadia a5 mtcal the same over Wi-Fi
#
# The calibration is per panel: iBoot copies it out of syscfg (MtCl) at every
# boot, and a running iOS is the only place to read it (tools/mtdump/mtcal.c).
# The image carries a default one, initramfs/lib/firmware/mtcal.bin, taken from
# the iPad this port was brought up on; this replaces it with the panel in
# hand, in build/keep/lib/firmware/mtcal.bin, which ./cascadia a5 build lays over
# the default.  So: this, then build, then flash.  The jailbroken iOS with
# OpenSSH is the one ./cascadia a5 flash --kdfu starts from anyway.  The
# digitizer's firmware is not fetched here: ./cascadia a5 firmware takes it out
# of the IPSW.
#
# Over USB this goes through usbmuxd (tools/ios-ssh.sh, lib/usbmux.py):
# part of macOS, the usbmuxd package on Linux.  One ssh connection is shared
# by every step, so iOS's root password is asked for once (Enter is "alpine",
# iOS's default).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KEEP="$ROOT/build/keep/lib/firmware"
HELPER="$ROOT/tools/mtdump/prebuilt/mtcal"

fail() { echo "error: $*" >&2; exit 1; }

[ -f "$HELPER" ] || fail "no $HELPER -- it is in the repository; a partial checkout?"

. "$ROOT/tools/ios-ssh.sh"
ios_setup mtcal
ios_check_model

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
echo "    Into the image and onto the iPad:  ./cascadia a5 build && ./cascadia a5 flash --kdfu"
