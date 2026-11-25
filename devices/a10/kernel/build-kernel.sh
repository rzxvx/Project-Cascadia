#!/usr/bin/env bash
# build-kernel.sh - the T8010 kernel for the iPad 7 (J171 / J172).
#
# Source: Pauli1Go/HoolockLinux at the commit pinned in sources.env. Config:
# HoolockLinux's config_16k (checked by sha256) + a10.fragment, olddefconfig.
# Our changes to the tree: kernel/patches/*.patch (git apply, in order), then
# the new files of kernel/overlay/ copied over the tree (the GPU's Apple
# backend, drivers/gpu/drm/imagination/pvr_apple*).
#
# Built like the A13 kernel: an Ubuntu 24.04 container of the host's own
# architecture, clang cross-compiling (LLVM=1 ARCH=arm64), the source in a
# Docker volume (the kernel tree needs a case-sensitive file system).
#
# Output: devices/a10/build/payload/{Image.gz,t8010-j171.dtb,t8010-j172.dtb},
#         devices/a10/build/payload/modules.tar.gz (lib/modules/<release>: the
#         GPU driver and what it needs; ./cascadia a10 modules puts it on the
#         iPad's root), devices/a10/build/kernel.config
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
A10="$(cd "$HERE/.." && pwd)"
. "$A10/sources.env"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN)}"
OUT="$A10/build/payload"
mkdir -p "$OUT"

curl -fsSL "$A10_CONFIG_URL" -o "$A10/build/config_16k"
if command -v sha256sum >/dev/null; then
    echo "$A10_CONFIG_SHA256  $A10/build/config_16k" | sha256sum -c -
else
    echo "$A10_CONFIG_SHA256  $A10/build/config_16k" | shasum -a 256 -c -
fi

docker volume create cascadia-a10-ksrc >/dev/null
docker run --rm \
  -v cascadia-a10-ksrc:/src \
  -v "$OUT":/out \
  -v "$A10/build":/a10build \
  -v "$HERE":/kernel:ro \
  -e JOBS="$JOBS" -e REPO="$A10_KERNEL_REPO" -e COMMIT="$A10_KERNEL_COMMIT" \
  -e HOST_IDS="$(id -u):$(id -g)" \
  ubuntu:24.04 bash -euc '
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends \
      clang lld llvm make flex bison bc libssl-dev libelf-dev \
      python3 kmod cpio gzip git ca-certificates device-tree-compiler >/dev/null
    cd /src
    if [ ! -d linux/.git ]; then
      git init -q linux
      git -C linux remote add origin "$REPO"
    fi
    cd linux
    git fetch -q --depth=1 origin "$COMMIT"
    git checkout -q -f FETCH_HEAD
    git clean -qfdx -e .config
    for p in /kernel/patches/*.patch; do
      [ -e "$p" ] || continue
      echo "apply $(basename "$p")"
      git apply "$p"
    done
    [ ! -d /kernel/overlay ] || cp -R /kernel/overlay/. .
    cp /a10build/config_16k .config
    ./scripts/kconfig/merge_config.sh -m .config /kernel/a10.fragment >/dev/null
    make -s LLVM=1 ARCH=arm64 olddefconfig
    # what the fragment asked for but did not get
    missing=0
    while IFS= read -r line; do
      case "$line" in CONFIG_*=*) ;; *) continue ;; esac
      grep -qxF "$line" .config || { echo "fragment: not set: $line"; missing=1; }
    done < /kernel/a10.fragment
    [ "$missing" = 0 ] || echo "(see above: some fragment options did not survive olddefconfig)"
    make -s -j"$JOBS" LLVM=1 ARCH=arm64 Image.gz modules \
      apple/t8010-j171.dtb apple/t8010-j172.dtb
    cp arch/arm64/boot/Image.gz /out/
    cp arch/arm64/boot/dts/apple/t8010-j171.dtb arch/arm64/boot/dts/apple/t8010-j172.dtb /out/
    rm -rf /tmp/mods
    make -s LLVM=1 ARCH=arm64 INSTALL_MOD_PATH=/tmp/mods INSTALL_MOD_STRIP=1 modules_install
    rm -f /tmp/mods/lib/modules/*/build
    tar -C /tmp/mods -czf /out/modules.tar.gz lib
    cp .config /a10build/kernel.config
    chown "$HOST_IDS" /out/* /a10build/kernel.config
  '
echo "kernel OK: $OUT/Image.gz, t8010-j171.dtb, t8010-j172.dtb"
