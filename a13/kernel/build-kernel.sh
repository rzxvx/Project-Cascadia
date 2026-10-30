#!/usr/bin/env bash
# build-kernel.sh - build the Hoolock t8030 arm64 kernel for N104AP (iPhone 11).
#
# Source: yhavry/Hoolock-linux, branch t8030, pinned below. Config: Hoolock's
# known-good A13 config_16k (fetched from yhavry/Hoolock-Docs-A13 at a pinned
# commit and checked by sha256) + n104.fragment. config_16k gives 16K pages,
# PTR_AUTH/BTI off and the Apple platform drivers built in; a `make defconfig`
# kernel (4K pages, PAC+BTI, Apple drivers as modules) hangs in early boot.
#
# Built in an arm64 Ubuntu 24.04 container (clang 18), the source in a Docker
# volume: the macOS file system is case-insensitive, which the kernel tree is
# not. On Apple silicon this runs natively.
#
# Output: a13/build/payload/{Image.gz,t8030-n104.dtb}, a13/build/kernel.config
set -euo pipefail

REPO=https://github.com/yhavry/Hoolock-linux.git
COMMIT=2f2662c87fff28b83eab6786f53be2c2670ba53f
CONFIG_URL=https://raw.githubusercontent.com/yhavry/Hoolock-Docs-A13/96a110bc680f085f5744bb97679abbf41e2f0df5/config_16k
CONFIG_SHA256=8e6133a576773dc455b6384a7473bce0a646dfb04779bee8f994f00fe7691472
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN)}"
HERE="$(cd "$(dirname "$0")" && pwd)"
A13="$(cd "$HERE/.." && pwd)"
OUT="$A13/build/payload"
mkdir -p "$OUT"

curl -fsSL "$CONFIG_URL" -o "$A13/build/config_16k"
echo "$CONFIG_SHA256  $A13/build/config_16k" | shasum -a 256 -c -

docker volume create hoolock-ksrc >/dev/null
docker run --rm --platform linux/arm64 \
  -v hoolock-ksrc:/src \
  -v "$OUT":/out \
  -v "$A13/build":/a13build \
  -v "$HERE":/kernel:ro \
  -v "$HERE/extras":/extras:ro \
  -e JOBS="$JOBS" -e REPO="$REPO" -e COMMIT="$COMMIT" \
  arm64v8/ubuntu:24.04 bash -euxc '
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends \
      clang lld llvm make flex bison bc libssl-dev libelf-dev \
      python3 kmod cpio gzip git ca-certificates device-tree-compiler
    cd /src
    if [ -d linux/.git ] && [ "$(git -C linux remote get-url origin)" != "$REPO" ]; then
      rm -rf linux
    fi
    [ -d linux/.git ] || git clone --depth=1 --branch t8030 "$REPO" linux
    cd linux
    git fetch --depth=1 origin "$COMMIT"
    git checkout -q -f "$COMMIT"
    sh /extras/apply.sh
    export ARCH=arm64 LLVM=1
    cp /a13build/config_16k .config
    ./scripts/kconfig/merge_config.sh -m .config /kernel/n104.fragment
    make olddefconfig
    grep -E "^CONFIG_(ARM64_16K_PAGES|USB_DWC3_APPLE|PHY_APPLE_AUSB|DRM_SIMPLEDRM|FONT_8x8)=y" .config
    if grep -E "^CONFIG_ARM64_(4K_PAGES|PTR_AUTH|BTI)=y" .config; then exit 1; fi
    cp .config /a13build/kernel.config
    make -j"$JOBS" Image.gz dtbs
    cp -v arch/arm64/boot/Image.gz arch/arm64/boot/dts/apple/t8030-n104.dtb /out/
    echo "KERNEL BUILD OK"
  '
ls -la "$OUT"
