#!/usr/bin/env bash
# build-m1n1.sh - fetch HoolockLinux/m1n1 (idevice) at a pinned commit, apply
# our T8030 patches, build m1n1-idevice.macho (CHAINLOADING=1).
#
# Needs: clang + lld (Homebrew llvm/lld on macOS), Rust, GNU make.
# Output: a13/build/payload/m1n1-idevice.macho
set -euo pipefail

REPO=https://github.com/HoolockLinux/m1n1.git
COMMIT=d5a10ac52a6468484854419a6c5130f1d62073eb   # branch idevice
HERE="$(cd "$(dirname "$0")" && pwd)"
A13="$(cd "$HERE/.." && pwd)"
SRC="$A13/build/m1n1"
OUT="$A13/build/payload"
mkdir -p "$OUT"

# shallow, no tags: the build tag then reads "d5a10ac-dirty", as on the
# builds tested on the device (a full clone gives "v1.6.0-137-g...")
if [ ! -d "$SRC/.git" ]; then
    git init -q "$SRC"
    git -C "$SRC" remote add origin "$REPO"
fi
cd "$SRC"
git fetch -q --depth=1 --no-tags origin "$COMMIT"
git checkout -q -f FETCH_HEAD
git submodule update --init --recursive
for p in "$HERE"/0*.patch; do
    git apply "$p"
done

MAKE=$(command -v gmake || command -v make)
"$MAKE" CHAINLOADING=1 build/m1n1-idevice.macho
cp build/m1n1-idevice.macho "$OUT/"
echo "m1n1 OK: $OUT/m1n1-idevice.macho"
