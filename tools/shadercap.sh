#!/usr/bin/env bash
#
# The shader oracle (M11, docs/research/p105-mesa.md): iOS's own GL driver
# compiles every shader of the corpus (tools/iosgpu/corpus/*.glsl) on this
# iPad's jailbroken iOS 8.4.1, and the GPU memory each draw changed comes back
# -- the USSE and PDS programs among it -- for tools/iosgpu/corpus.py to read.
#
#   bash tools/shadercap.sh                        iPad booted into iOS, on USB
#   IOS_HOST=192.168.1.20 bash tools/shadercap.sh  the same over Wi-Fi
#   bash tools/shadercap.sh f0*.glsl               only some cases (names in
#                                                  tools/iosgpu/corpus)
#
# gltrace has to be built with the corpus mode (tools/iosgpu/build.sh: macOS
# with Xcode and ldid); this builds it when build/gltrace is missing or older
# than gltrace.m.  The connection is gpucap.sh's: usbmuxd and iproxy over
# USB, or IOS_HOST= over Wi-Fi; iOS's root password, "alpine" unless
# changed.  The result goes to logs/ios/corpus/<date>/ (out of git: it is
# Apple's compiler's output), a few MB.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIK="${LIK:-$HOME/Legacy-iOS-Kit}"
HELPER="$ROOT/build/gltrace"
CORPUS="$ROOT/tools/iosgpu/corpus"
OUT="$ROOT/logs/ios/corpus/$(date +%Y%m%d-%H%M%S)"

fail() { echo "error: $*" >&2; exit 1; }

if [ ! -f "$HELPER" ] || [ "$ROOT/tools/iosgpu/gltrace.m" -nt "$HELPER" ]; then
    [ "$(uname -s)" = Darwin ] || fail "build/gltrace is missing or old, and it only builds on macOS
  (tools/iosgpu/build.sh, with Xcode and ldid); build it there and copy it to $HELPER"
    command -v ldid >/dev/null 2>&1 || fail "no ldid (brew install ldid) -- tools/iosgpu/build.sh signs with it"
    echo "==> building gltrace (tools/iosgpu/build.sh)"
    sh "$ROOT/tools/iosgpu/build.sh" || fail "gltrace did not build"
fi

cases=()
if [ $# -gt 0 ]; then
    for c in "$@"; do
        for f in "$CORPUS"/$c; do
            [ -f "$f" ] || f="$CORPUS/$c"
            [ -f "$f" ] || fail "no such case: $c"
            cases+=("$f")
        done
    done
else
    cases=("$CORPUS"/*.glsl)
fi
[ ${#cases[@]} -gt 0 ] || fail "no cases in $CORPUS (python3 tools/iosgpu/corpus/make.py writes them)"

# /tmp and not $TMPDIR: the ssh control socket's path has to fit in a
# sockaddr_un, and macOS's TMPDIR alone takes half of it.
WORK=$(mktemp -d /tmp/cascadia-shadercap.XXXXXX)
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
  macOS: brew install libimobiledevice     Or over Wi-Fi: IOS_HOST=<the iPad's IP>"
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
[ -z "$build" ] || [ "$build" = "12H321" ] || echo "warning: iOS build $build, not 8.4.1's 12H321 -- another compiler"
echo "    $model ${build:-}"

# A fresh path every run: iOS caches a binary's signature by vnode, and a
# file rewritten in place, or a path re-used, is killed on sight ("Killed: 9").
D="/var/root/shadercap.$$"
BIN="$D/gltrace"
ios "rm -rf $D && mkdir -p $D/in $D/out && cat > $BIN && chmod 755 $BIN" < "$HELPER"
echo "==> ${#cases[@]} cases to the iPad"
for f in "${cases[@]}"; do
    ios "cat > $D/in/$(basename "$f")" < "$f"
done

echo "==> drawing them with iOS's GL driver (gltrace corpus)"
ios "cd $D && ./gltrace corpus $D/out in/*.glsl" > "$WORK/log.txt" 2>&1 || true
grep '^== \|does not\|drawn:' "$WORK/log.txt" | sed 's/^/    /' | tail -n 400 | grep -v '^    == case' || true
grep -q '^== corpus done' "$WORK/log.txt" \
    || { ios rm -rf "$D" || true; fail "gltrace stopped before the end; its output: $WORK/log.txt (kept below)
$(tail -20 "$WORK/log.txt")"; }

echo "==> fetching what changed"
mkdir -p "$OUT"
cp "$WORK/log.txt" "$OUT/"
for f in $(ios "cd $D/out && ls"); do
    ios "cat $D/out/$f" > "$OUT/$f"
    n=$(wc -c < "$OUT/$f" | tr -d ' ')
    [ $((n % 4100)) -eq 0 ] || fail "$f came back cut off ($n bytes) -- try again (Wi-Fi from a Mac: IOS_HOST=)"
done
ios rm -rf "$D"
echo "    $(ls "$OUT" | wc -l | tr -d ' ') files, $(du -sh "$OUT" | cut -f1) in ${OUT#$ROOT/}"

echo "==> reading the programs (tools/iosgpu/corpus.py)"
python3 "$ROOT/tools/iosgpu/corpus.py" "$OUT" > "$OUT/programs.txt" \
    && echo "    ${OUT#$ROOT/}/programs.txt" \
    || echo "    corpus.py failed; the capture is in ${OUT#$ROOT/}"
