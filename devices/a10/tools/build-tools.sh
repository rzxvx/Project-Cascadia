#!/usr/bin/env bash
# build-tools.sh - fetch and build the host side of the boot chain at the
# pinned commits (sources.env): gaster (checkm8), hBootPatcher, makez2fw (the
# touch firmware converter) and hcdpack (the Bluetooth firmware extractor).
# Output: devices/a10/build/bin/{gaster,hBootPatcher,makez2fw,hcdpack}
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
A10="$(cd "$HERE/.." && pwd)"
. "$A10/sources.env"
BIN="$A10/build/bin"
mkdir -p "$BIN" "$A10/build/src"
MAKE=$(command -v gmake || command -v make)

fetch() {   # repo commit dir
    local dir="$A10/build/src/$3"
    if [ ! -d "$dir/.git" ]; then
        git init -q "$dir"
        git -C "$dir" remote add origin "$1"
    fi
    git -C "$dir" fetch -q --depth=1 origin "$2"
    git -C "$dir" checkout -q -f FETCH_HEAD
    # the Hoolock repos name their submodules by SSH URL; fetch them over HTTPS
    git -C "$dir" -c url."https://github.com/".insteadOf=git@github.com: \
        submodule update -q --init --recursive
}

fetch "$A10_GASTER_REPO" "$A10_GASTER_COMMIT" gaster
if [ "$(uname -s)" = Darwin ]; then
    "$MAKE" -s -C "$A10/build/src/gaster" macos
else
    "$MAKE" -s -C "$A10/build/src/gaster" libusb_dyn
fi
cp "$A10/build/src/gaster/gaster" "$BIN/"

fetch "$A10_HBOOTPATCHER_REPO" "$A10_HBOOTPATCHER_COMMIT" hBootPatcher
"$MAKE" -s -C "$A10/build/src/hBootPatcher"
cp "$A10/build/src/hBootPatcher/hBootPatcher" "$BIN/"

fetch "$A10_MAKEZ2FW_REPO" "$A10_MAKEZ2FW_COMMIT" makez2fw
"$MAKE" -s -C "$A10/build/src/makez2fw/makez2fw" CFLAGS="-O2 -w" MXML_CFLAGS="-O2 -w"
cp "$A10/build/src/makez2fw/makez2fw/makez2fw" "$BIN/"

mkdir -p "$A10/build/src/hcdpack"
curl -fsSL "$A10_HCDPACK_URL" -o "$A10/build/src/hcdpack/hcdpack.c"
echo "$A10_HCDPACK_SHA256  $A10/build/src/hcdpack/hcdpack.c" | shasum -a 256 -c - >/dev/null \
    || { echo "error: hcdpack.c checksum" >&2; exit 1; }
cc -O2 -w -D_DARWIN_C_SOURCE -o "$BIN/hcdpack" "$A10/build/src/hcdpack/hcdpack.c"

echo "tools OK: $BIN/gaster $BIN/hBootPatcher $BIN/makez2fw $BIN/hcdpack"
