#!/usr/bin/env bash
#
# A capture of what iOS's GL driver sets up for a textured quad, from this
# iPad's own jailbroken iOS -- a research tool.  ./cascadia gpu does not need
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
# The connection is the one ./cascadia mtcal uses: usbmuxd and iproxy over
# USB, or IOS_HOST= over Wi-Fi; iOS's root password, "alpine" unless changed.
# About 5 MB comes back.  If a USB transfer from a Mac stops halfway, use
# Wi-Fi (Apple's usbmuxd has been seen to reset long transfers).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIK="${LIK:-$HOME/Legacy-iOS-Kit}"
HELPER="$ROOT/tools/iosgpu/prebuilt/gltrace"
OUT="$ROOT/logs/ios/mod"

fail() { echo "error: $*" >&2; exit 1; }

[ -f "$HELPER" ] || fail "no $HELPER -- it is in the repository; a partial checkout?"

# /tmp and not $TMPDIR: the ssh control socket's path has to fit in a
# sockaddr_un, and macOS's TMPDIR alone takes half of it.
WORK=$(mktemp -d /tmp/cascadia-gpucap.XXXXXX)
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
    # The first free port from 2222: one may be taken by an iproxy left
    # running from before -- seen on the Arch box, which then failed here
    # with "Address already in use".
    HOST=127.0.0.1; PORT="${IOS_PORT:-$(python3 -c '
import socket
for p in range(2222, 2300):
    s = socket.socket()
    try:
        s.bind(("", p)); print(p); break
    except OSError:
        pass
    finally:
        s.close()')}"
    # The first iproxy that runs, PATH's then Legacy iOS Kit's: Homebrew's
    # has been seen broken by a libplist upgrade under it ("Library not
    # loaded: libplist-2.0.4.dylib", abort trap) -- brew reinstall libusbmuxd.
    IPROXY=""
    case "$(uname -s)/$(uname -m)" in
        Darwin/*)                     lik="$LIK/bin/macos/iproxy" ;;
        Linux/aarch64|Linux/arm64)    lik="$LIK/bin/linux/arm64/iproxy" ;;
        *)                            lik="$LIK/bin/linux/x86_64/iproxy" ;;
    esac
    for c in "$(command -v iproxy || true)" "$lik"; do
        [ -n "$c" ] && [ -x "$c" ] || continue
        rc=$( { "$c" -h >/dev/null 2>&1; echo $?; } 2>/dev/null )
        if [ "$rc" -lt 127 ]; then IPROXY="$c"; break; fi
        echo "    $c does not run (exit $rc); trying the next one" >&2
    done
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

ios() {
    ssh -F "$ROOT/tools/mtdump/ssh_config" -o ControlMaster=auto -o ControlPath="$CM" \
        -o ControlPersist=300 -o ConnectTimeout=10 -p "$PORT" "root@$HOST" "$@"
}

echo "==> connecting (iOS root password, 'alpine' unless changed)"
model=$(ios uname -m) || fail "no ssh to iOS on $HOST:$PORT -- is the iPad in iOS, unlocked, with OpenSSH installed?"
[ "$model" = "iPad2,5" ] || fail "this is a $model; this port is for iPad2,5 (p105ap)"
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
