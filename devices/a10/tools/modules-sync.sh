#!/usr/bin/env bash
# modules-sync.sh - ./cascadia a10 modules [gpu]: the kernel's modules and the
# GPU's firmware files onto the running iPad's root (the NVMe root has none of
# the initramfs's /lib/firmware).
#
#   ./cascadia a10 modules        build/payload/modules.tar.gz (build-kernel.sh)
#                                 -> /lib/modules/<release>; the GFX firmware and
#                                 the self-test's shaders -> /lib/firmware/apple
#   ./cascadia a10 modules gpu    rebuild just powervr.ko in the kernel volume
#                                 (the tree of the last full build): the
#                                 patches' drm/imagination parts re-applied, the
#                                 overlay on top; push it, reload it
#
# The running kernel must be the one built with these modules (same release,
# same config): after build-kernel.sh, pack and boot it first.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
A10="$(cd "$HERE/.." && pwd)"
HOST=${HOST:-root@172.16.43.1}
ipad() { ssh -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR "$HOST" "$@"; }

ipad true </dev/null 2>/dev/null || { echo "error: no Linux on the iPad at ${HOST#*@}" >&2; exit 1; }
rel=$(ipad uname -r </dev/null)

firmware() {
    echo "==> /lib/firmware/apple: the GFX firmware, the self-test's shaders"
    [ -f "$A10/build/firmware/apple/gfx-t8010-22H355.bin" ] || bash "$HERE/gpu/gfx-fw.sh"
    python3 "$HERE/gpu/g9p-selftest.py" >/dev/null
    ( cd "$A10/build/firmware" && COPYFILE_DISABLE=1 tar --format ustar --no-xattrs --uid 0 --gid 0 \
        --uname root --gname root -cf - apple/gfx-t8010-22H355.bin apple/g9p-selftest.bin ) \
        | ipad 'mkdir -p /lib/firmware && tar xf - -C /lib/firmware && sync'
}

case "${1:-}" in
"")
    tgz="$A10/build/payload/modules.tar.gz"
    [ -f "$tgz" ] || { echo "error: no $tgz (./cascadia a10 build kernel)" >&2; exit 1; }
    have=$(tar tzf "$tgz" | sed -n 's|^lib/modules/\([^/]*\)/$|\1|p' | head -1)
    [ "$have" = "$rel" ] || { echo "error: modules for $have, the iPad runs $rel (pack + boot the new kernel)" >&2; exit 1; }
    echo "==> /lib/modules/$rel"
    ipad "rm -rf /lib/modules/$rel && tar xzf - -C / && sync" < "$tgz"
    firmware
    ;;
gpu)
    echo "==> powervr.ko from kernel/overlay (kernel volume, incremental)"
    docker run --rm -v cascadia-a10-ksrc:/src -v "$A10/kernel":/kernel:ro -v "$A10/build/payload":/out \
        -e HOST_IDS="$(id -u):$(id -g)" -e REL="$rel" ubuntu:24.04 bash -euc '
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y -qq --no-install-recommends clang lld llvm make flex bison bc libssl-dev \
          libelf-dev python3 kmod git ca-certificates >/dev/null 2>&1
        cd /src/linux
        git config --global --add safe.directory "*"
        # the driver as the patches have it now (their imagination/ parts), then the overlay
        git checkout -q -- drivers/gpu/drm/imagination
        for p in /kernel/patches/*.patch; do
            grep -q "^+++ b/drivers/gpu/drm/imagination/" "$p" &&
                git apply --include="drivers/gpu/drm/imagination/*" "$p"
        done
        cp -R /kernel/overlay/. .
        # the release string of the running kernel (vermagic must match it)
        make -s LLVM=1 ARCH=arm64 KERNELRELEASE="$REL" prepare
        make -s -j8 LLVM=1 ARCH=arm64 KERNELRELEASE="$REL" M=drivers/gpu/drm/imagination modules
        llvm-strip --strip-debug -o /out/powervr.ko drivers/gpu/drm/imagination/powervr.ko
        chown "$HOST_IDS" /out/powervr.ko'
    firmware
    echo "==> reload powervr"
    ipad "rmmod powervr 2>/dev/null; cat > /lib/modules/$rel/kernel/drivers/gpu/drm/imagination/powervr.ko && sync &&
          modprobe powervr && dmesg | grep -i -E 'powervr|pvr|gfx|rtkit' | tail -8" < "$A10/build/payload/powervr.ko"
    ;;
*) echo "error: modules [gpu]" >&2; exit 1 ;;
esac
echo "==> done (self-tests: mount -t debugfs none /sys/kernel/debug; echo store > /sys/kernel/debug/pvr-apple/selftest)"
