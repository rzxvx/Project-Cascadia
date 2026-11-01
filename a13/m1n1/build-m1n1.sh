#!/usr/bin/env bash
# build-m1n1.sh - fetch HoolockLinux/m1n1 (idevice) at a pinned commit, apply
# our T8030 patches, build m1n1-idevice.macho (CHAINLOADING=1).
#
# Toolchain: on macOS the host's -- Homebrew llvm + lld, Rust with the
# aarch64-unknown-none-softfloat target, GNU make (the builds tested on the
# phone). Elsewhere (or M1N1_DOCKER=1) a container: Debian 13's clang / lld and
# rustup -- a distribution's Rust lacks the bare-metal target. Only git is
# needed on the host then.
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

if [ "${M1N1_DOCKER:-}" = 1 ] || { [ -z "${M1N1_DOCKER:-}" ] && [ "$(uname -s)" != Darwin ]; }; then
    IMAGE=cascadia-m1n1-build:trixie
    docker build -q ${DOCKER_PLATFORM:+--platform "$DOCKER_PLATFORM"} -t "$IMAGE" - >/dev/null <<'EOF'
FROM debian:trixie
RUN apt-get update && apt-get install -y --no-install-recommends \
      clang lld llvm make git curl ca-certificates libc6-dev python3 \
    && rm -rf /var/lib/apt/lists/*
RUN curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal \
      --target aarch64-unknown-none-softfloat
ENV PATH=/root/.cargo/bin:$PATH
EOF
    # build/ from scratch: a host build leaves host-built Rust build scripts
    docker run --rm ${DOCKER_PLATFORM:+--platform "$DOCKER_PLATFORM"} \
        -v "$SRC":/m1n1 -w /m1n1 -e HOST_IDS="$(id -u):$(id -g)" "$IMAGE" sh -ec '
        git config --global --add safe.directory "*"
        rm -rf build
        # clang as on macOS (m1n1 picks it only on Darwin by itself)
        tc=$(llvm-config --bindir)/
        make USE_CLANG=1 ARCH=aarch64-linux-gnu- TOOLCHAIN="$tc" LLDDIR="$tc" \
            CHAINLOADING=1 build/m1n1-idevice.macho
        chown -R "$HOST_IDS" build'
else
    rm -rf build        # a container build leaves Linux-built Rust build scripts
    MAKE=$(command -v gmake || command -v make)
    "$MAKE" CHAINLOADING=1 build/m1n1-idevice.macho
fi
cp build/m1n1-idevice.macho "$OUT/"
echo "m1n1 OK: $OUT/m1n1-idevice.macho"
