#!/usr/bin/env bash
#
# Put Mesa's source at build/mesa/src, pinned to the release the driver is
# written against.  ./cascadia mesa runs it on the host before the build.
#
#   bash mesa/fetch.sh
#   MESA_URL=https://... bash mesa/fetch.sh     # another mirror of it
#
# On the host, not in the cascadia-mesa image: git there runs under ARM
# emulation, and the first ./cascadia mesa on a Mac sat in its clone for
# over an hour with nothing to show.  Here it is native and shows progress,
# a stalled download gives up after a minute instead of hanging, and a
# mirror is tried when freedesktop.org does not answer.  Any mirror will
# do: the tree is checked by commit, so a tag that moved is refused.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/build/mesa/src"
TAG=mesa-26.1.8
COMMIT=0fadfea4f394211946f308458f614839ef253ee8     # mesa/build.sh reads this
URLS="${MESA_URL:-https://gitlab.freedesktop.org/mesa/mesa.git https://github.com/chaotic-cx/mesa-mirror.git}"

fail() { echo "error: $*" >&2; exit 1; }

command -v git >/dev/null 2>&1 || fail "git is not installed"

at_pin() { [ "$(git -c safe.directory="$SRC" -C "$SRC" rev-parse -q --verify HEAD 2>/dev/null)" = "$COMMIT" ]; }

if at_pin; then
    echo "==> build/mesa/src already present ($TAG)"
    exit 0
fi

mkdir -p "$ROOT/build/mesa"
for url in $URLS; do
    echo "==> cloning Mesa $TAG from ${url#https://}"
    echo "    (shallow: one commit, ~120 MB to download)"
    rm -rf "$SRC"
    # slower than 10 kB/s for a minute is a stall, not a slow line
    if git -c advice.detachedHead=false -c http.lowSpeedLimit=10000 -c http.lowSpeedTime=60 \
           clone --depth 1 --branch "$TAG" "$url" "$SRC"; then
        at_pin && { echo "==> done: Mesa $TAG at build/mesa/src"; exit 0; }
        echo "    $TAG there is not $COMMIT -- not using it" >&2
    fi
done
rm -rf "$SRC"
fail "could not fetch Mesa $TAG.  Clone it yourself into build/mesa/src
       (git clone --depth 1 --branch $TAG URL build/mesa/src), or name a
       mirror: MESA_URL=URL ./cascadia mesa"
