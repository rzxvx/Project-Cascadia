#!/usr/bin/env bash
# Mesa's source at build/mesa/src, pinned (the release the A5's driver is
# written against too). Cloned from the A5's tree when it is there (no
# download), else from the network. On the host: git in the image is slow.
set -euo pipefail
A10="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$A10/build/mesa/src"
COMMIT=0fadfea4f394211946f308458f614839ef253ee8     # mesa-26.1.8; build.sh reads this
A5SRC="$A10/../a5/build/mesa/src"
URL=${MESA_URL:-https://gitlab.freedesktop.org/mesa/mesa.git}

if [ "$(git -C "$SRC" rev-parse -q --verify HEAD 2>/dev/null)" = "$COMMIT" ]; then
    echo "==> build/mesa/src present"; exit 0
fi
mkdir -p "$A10/build/mesa"
rm -rf "$SRC"
if [ -d "$A5SRC/.git" ]; then
    git clone -q --shared --no-checkout "$A5SRC" "$SRC"
else
    git clone -q --depth 1 --branch mesa-26.1.8 --no-checkout "$URL" "$SRC"
fi
git -c advice.detachedHead=false -C "$SRC" checkout -q "$COMMIT"
echo "==> build/mesa/src at $COMMIT"
