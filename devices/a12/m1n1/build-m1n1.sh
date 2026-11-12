#!/usr/bin/env bash
# Build the XR/T8020 m1n1 payload by reusing Cascadia's tested A13 downstream
# stack and applying the small A12 overlay last.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
A12="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$A12/.." && pwd)"
SRC="$A12/build/m1n1"
source "$A12/profile.sh"
BOARD="$A12_BOARD"
OUT="$A12_PAYLOAD"
REPO=https://github.com/HoolockLinux/m1n1.git
COMMIT=d5a10ac52a6468484854419a6c5130f1d62073eb

mkdir -p "$SRC" "$OUT"
if [ ! -d "$SRC/.git" ]; then
    git init -q "$SRC"
    git -C "$SRC" remote add origin "$REPO"
fi
git -C "$SRC" fetch -q --depth=1 --no-tags origin "$COMMIT"
git -C "$SRC" checkout -q -f FETCH_HEAD
# This is an ignored, script-owned build tree.  A previous build leaves files
# added by the patch series, so remove those too before applying it again.
git -C "$SRC" clean -qfdx
git -C "$SRC" submodule update --init --recursive

if [ "$BOARD" = d331p ]; then
    for p in "$HERE"/d331p/*.patch; do git -C "$SRC" apply "$p"; done
else
    for p in "$HERE"/base/0*.patch; do git -C "$SRC" apply "$p"; done
    for p in "$HERE"/0*.patch; do git -C "$SRC" apply "$p"; done
fi

# Docker by default on Linux, when it is there. M1N1_DOCKER=0 builds natively.
if [ "${M1N1_DOCKER:-}" = 1 ] || { [ -z "${M1N1_DOCKER:-}" ] && [ "$(uname -s)" != Darwin ] &&
    command -v docker >/dev/null; }; then
    IMAGE=cascadia-m1n1-build:trixie
    docker build -q -t "$IMAGE" - >/dev/null <<'EOF'
FROM debian:trixie
RUN apt-get update && apt-get install -y --no-install-recommends clang lld llvm make git curl ca-certificates libc6-dev python3 && rm -rf /var/lib/apt/lists/*
RUN curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal --target aarch64-unknown-none-softfloat
ENV PATH=/root/.cargo/bin:$PATH
EOF
    docker run --rm -v "$SRC":/m1n1 -w /m1n1 \
        -e HOST_IDS="$(id -u):$(id -g)" "$IMAGE" sh -ec '
        git config --global --add safe.directory "*"
        rm -rf build
        tc=$(llvm-config --bindir)/
        make USE_CLANG=1 ARCH=aarch64-linux-gnu- TOOLCHAIN="$tc" LLDDIR="$tc" \
            CHAINLOADING=1 build/m1n1-idevice.macho
        chown -R "$HOST_IDS" build'
else
    rm -rf "$SRC/build"
    MAKE=$(command -v gmake || command -v make)
    # On Linux, clang cross compiles and BUILDSTD builds Rust's core from
    # source, so neither a cross gcc nor rustup's target is needed. RELEASE
    # because the debug build pulls in wcslen.
    native=
    [ "$(uname -s)" = Darwin ] || native="USE_CLANG=1 BUILDSTD=1 RELEASE=1"
    "$MAKE" -C "$SRC" CHAINLOADING=1 $native build/m1n1-idevice.macho
fi

cp "$SRC/build/m1n1-idevice.macho" "$OUT/m1n1-idevice.macho"
echo "$BOARD m1n1 OK: $OUT/m1n1-idevice.macho"
