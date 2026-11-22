#!/usr/bin/env bash
# build-alpine-rootfs.sh - the Alpine Linux (aarch64) root for the iPad 7, as
# the A13 track builds it (devices/a13/rootfs): apk in an arm64 Alpine
# container, packed as a tarball so owners, modes and setuid bits survive.
# BusyBox init with our own inittab (overlay/), not OpenRC: the initramfs has
# done the boot; a10-init only starts the services.
#
# root has no password: the USB link and the screen are yours (as the
# initramfs). pack sends the tarball as an iBoot ramdisk, and /init unpacks
# it into RAM.
#
# Output: devices/a10/build/alpine-a10.tar.gz (RAM), alpine-a10-desktop.tar.gz (NVMe)
set -euo pipefail

ALPINE_VER="${ALPINE_VER:-3.24}"
A10="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$A10/build"
# Two roots from one recipe: rootfs/packages, small enough to travel as an
# iBoot ramdisk (Alpine in RAM), and the same with rootfs/packages-desktop
# (labwc and the rest, Mesa's llvmpipe) for the NVMe (./cascadia a10 nvme
# format). ./cascadia a10 rootfs sync installs both lists on a running root.
BASE="$(tr '\n' ' ' < "$A10/rootfs/packages")"
DESKTOP="$(tr '\n' ' ' < "$A10/rootfs/packages-desktop")"

# task-killer (tools/task-killer): built here, carried in /usr/local/bin
[ -x "$A10/build/task-killer/task-killer" ] || sh "$A10/tools/task-killer/build.sh"

build() {   # tarball packages...
docker run --rm --platform linux/arm64 \
  -v "$A10/rootfs/overlay":/overlay:ro \
  -v "$A10/build":/dist \
  -v "$A10/build/task-killer":/tk:ro \
  -e HOST_IDS="$(id -u):$(id -g)" -e OUT="$1" -e PACKAGES="$2" \
  "alpine:$ALPINE_VER" sh -euc '
    mkdir /r
    apk add -q --root /r --initdb --keys-dir /etc/apk/keys \
        --repositories-file /overlay/etc/apk/repositories $PACKAGES
    cp -a /overlay/. /r/
    # owned by root, whatever owns them on the host
    (cd /overlay && find . -mindepth 1) | while read -r f; do chown -h root:root "/r/$f"; done
    sed -i "s/^root:[^:]*:/root::/" /r/etc/shadow
    cp /tk/task-killer /r/usr/local/bin/task-killer
    chmod 755 /r/sbin/a10-init /r/usr/local/bin/*
    mkdir -p /r/root/.ssh /r/etc/dropbear
    chmod 700 /r/root /r/root/.ssh
    tar -C /r -czf "/dist/$OUT" .
    chown $HOST_IDS "/dist/$OUT"
  '
ls -la "$A10/build/$1"
}
build alpine-a10.tar.gz "$BASE"
build alpine-a10-desktop.tar.gz "$BASE $DESKTOP"
