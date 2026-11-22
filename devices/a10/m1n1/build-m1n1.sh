#!/usr/bin/env bash
# build-m1n1.sh - HoolockLinux/m1n1 (idevice) at the pinned commit, our T8010
# patches (m1n1/0*.patch, none yet), m1n1-idevice.macho with CHAINLOADING=1.
# The same commit and build as devices/a13/m1n1/build-m1n1.sh, without the
# A13 patches. macOS: the host's Homebrew llvm + lld + Rust; elsewhere (or
# M1N1_DOCKER=1) the A13 track's container.
# Output: devices/a10/build/payload/m1n1-idevice.macho
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
A10="$(cd "$HERE/.." && pwd)"
. "$A10/sources.env"
SRC="$A10/build/m1n1"
OUT="$A10/build/payload"
mkdir -p "$OUT"

if [ ! -d "$SRC/.git" ]; then
    git init -q "$SRC"
    git -C "$SRC" remote add origin "$A10_M1N1_REPO"
fi
cd "$SRC"
git fetch -q --depth=1 --no-tags origin "$A10_M1N1_COMMIT"
git checkout -q -f FETCH_HEAD
git submodule update -q --init --recursive
for p in "$HERE"/0*.patch; do
    [ -e "$p" ] || continue
    git apply "$p"
done

if [ "${M1N1_DOCKER:-}" = 1 ] || { [ -z "${M1N1_DOCKER:-}" ] && [ "$(uname -s)" != Darwin ]; }; then
    IMAGE=cascadia-m1n1-build:trixie
    docker build -q -t "$IMAGE" - >/dev/null <<'EOF'
FROM debian:trixie
RUN apt-get update && apt-get install -y --no-install-recommends \
      clang lld llvm make git curl ca-certificates libc6-dev python3 \
    && rm -rf /var/lib/apt/lists/*
RUN curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal \
      --target aarch64-unknown-none-softfloat
ENV PATH=/root/.cargo/bin:$PATH
EOF
    docker run --rm -v "$SRC":/m1n1 -w /m1n1 -e HOST_IDS="$(id -u):$(id -g)" "$IMAGE" sh -ec '
        git config --global --add safe.directory "*"
        rm -rf build
        tc=$(llvm-config --bindir)/
        make USE_CLANG=1 ARCH=aarch64-linux-gnu- TOOLCHAIN="$tc" LLDDIR="$tc" \
            CHAINLOADING=1 build/m1n1-idevice.macho
        chown -R "$HOST_IDS" build'
else
    rm -rf build
    MAKE=$(command -v gmake || command -v make)
    "$MAKE" CHAINLOADING=1 build/m1n1-idevice.macho
fi
cp build/m1n1-idevice.macho "$OUT/"
echo "m1n1 OK: $OUT/m1n1-idevice.macho"
