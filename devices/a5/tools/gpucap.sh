#!/usr/bin/env bash
#
# A capture of what iOS's GL driver sets up for a textured quad, from this
# iPad's own jailbroken iOS -- a research tool.  ./cascadia a5 dev gpu does not need
# it any more: tools/sgx/frame.py builds the same frame from our own programs
# (frame.py --check compares the two, word by word).
#
#   bash tools/gpucap.sh                       iPad booted into its jailbroken iOS, on USB
#   IOS_HOST=192.168.1.20 bash tools/gpucap.sh the same over Wi-Fi
#
# It runs tools/iosgpu/gltrace on the iPad -- it draws a textured quad with
# GLES2 in three blend modes and saves the GPU memory the GL driver set up
# for it -- and brings the result back into logs/ios/mod/ (out of git: it is
# Apple's code and data).
#
# The helper is prebuilt (tools/iosgpu/prebuilt/gltrace, from gltrace.m by
# tools/iosgpu/build.sh, which needs Xcode), so this works from any host.
# The connection is the one ./cascadia a5 mtcal uses (tools/ios-ssh.sh): usbmuxd
# over USB, or IOS_HOST= over Wi-Fi; iOS's root password, "alpine" unless changed.
# About 5 MB comes back.  If a USB transfer from a Mac stops halfway, use
# Wi-Fi (Apple's usbmuxd has been seen to reset long transfers).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HELPER="$ROOT/tools/iosgpu/prebuilt/gltrace"
OUT="$ROOT/logs/ios/mod"

fail() { echo "error: $*" >&2; exit 1; }

[ -f "$HELPER" ] || fail "no $HELPER -- it is in the repository; a partial checkout?"

. "$ROOT/tools/ios-ssh.sh"
ios_setup gpucap
ios_check_model
model=$IOS_MODEL
build=$(ios sysctl -n kern.osversion 2>/dev/null || true)
[ -z "$build" ] || [ "$build" = "12H321" ] || fail "iOS build $build; the templates are taken from 8.4.1 (12H321)"
echo "    $model ${build:-}"

# A fresh path every run: iOS caches a binary's signature by vnode, and a
# file rewritten in place, or a path re-used, is killed on sight ("Killed: 9").
BIN="/tmp/gltrace.$$"
ios "rm -f $BIN && cat > $BIN && chmod 755 $BIN" < "$HELPER"
echo "==> drawing on the iPad (gltrace mod)"
ios "rm -f /var/root/gt_m_blend.bin /var/root/gt_m_add.bin /var/root/gt_m_mod.bin; $BIN mod" \
    > "$WORK/gtm.out" || { ios rm -f "$BIN" || true; fail "gltrace failed on the iPad (its output is above)"; }
grep -q '== mod_blend: render command at' "$WORK/gtm.out" \
    || fail "gltrace ran but did not capture the render command; its output: $WORK/gtm.out"
echo "==> fetching the capture"
ios cat /var/root/gt_m_blend.bin > "$WORK/gt_m_blend.bin"
ios rm -f "$BIN" /var/root/gt_m_blend.bin /var/root/gt_m_add.bin /var/root/gt_m_mod.bin

# The file is a run of (lo, hi, bytes[hi-lo]) regions; walking it to the end
# checks a transfer that stopped halfway.
python3 - "$WORK/gt_m_blend.bin" <<'EOF' || fail "the capture came back truncated -- try again (over Wi-Fi from a Mac)"
import struct, sys
d = open(sys.argv[1], 'rb').read()
o = n = 0
while o < len(d):
    lo, hi = struct.unpack_from('<II', d, o)
    o += 8 + hi - lo
    n += 1
assert n and o == len(d)
print('    %d regions, %d bytes' % (n, len(d)))
EOF

mkdir -p "$OUT"
cp "$WORK/gtm.out" "$WORK/gt_m_blend.bin" "$OUT/"
echo "==> logs/ios/mod/: gt_m_blend.bin, gtm.out"
echo
echo "    Compare with ours: python3 tools/sgx/frame.py build/sgx2d/frame --check <canonical capture>"
