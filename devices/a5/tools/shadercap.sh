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
#                                                  tools/iosgpu/corpus; the
#                                                  GLES 1 cases then not)
#
# gltrace has to be built with the corpus mode (tools/iosgpu/build.sh: macOS
# with Xcode and ldid); this builds it when build/gltrace is missing or older
# than gltrace.m.  The connection is gpucap.sh's (tools/ios-ssh.sh): usbmuxd over
# USB, or IOS_HOST= over Wi-Fi; iOS's root password, "alpine" unless
# changed.  The result goes to logs/ios/corpus/<date>/ (out of git: it is
# Apple's compiler's output), a few MB.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
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

. "$ROOT/tools/ios-ssh.sh"
ios_setup shadercap
ios_check_model
model=$IOS_MODEL
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
echo "    $(ls "$OUT" | wc -l | tr -d ' ') files, $(du -sh "$OUT" | cut -f1) in ${OUT#$ROOT/}"

# OpenGL ES 1's fixed function (M35: flat shading, logic ops, the alpha
# test, clip planes, point sprites, fog): gltrace's es1 mode, its own cases,
# into $OUT/es1/.  SHADERCAP_ES1=0: not.
if [ "${SHADERCAP_ES1:-1}" != 0 ] && [ $# -eq 0 ]; then
    echo "==> OpenGL ES 1's fixed function (gltrace es1)"
    ios "mkdir -p $D/es1 && cd $D && ./gltrace es1 $D/es1" > "$WORK/es1.txt" 2>&1 || true
    grep '^== es1\|drawn:' "$WORK/es1.txt" | sed 's/^/    /' | tail -n 60 || true
    mkdir -p "$OUT/es1"
    cp "$WORK/es1.txt" "$OUT/es1/log.txt"
    for f in $(ios "cd $D/es1 && ls"); do
        ios "cat $D/es1/$f" > "$OUT/es1/$f"
    done
fi
ios rm -rf "$D"

echo "==> reading the programs (tools/iosgpu/corpus.py)"
python3 "$ROOT/tools/iosgpu/corpus.py" "$OUT" > "$OUT/programs.txt" \
    && echo "    ${OUT#$ROOT/}/programs.txt" \
    || echo "    corpus.py failed; the capture is in ${OUT#$ROOT/}"
