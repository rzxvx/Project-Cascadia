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
# Output: devices/a10/build/alpine-a10.tar.gz
set -euo pipefail

ALPINE_VER="${ALPINE_VER:-3.24}"
PACKAGES="alpine-base dropbear dropbear-scp openssh-sftp-server htop nano eudev e2fsprogs"
A10="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$A10/build"

docker run --rm --platform linux/arm64 \
  -v "$A10/rootfs/overlay":/overlay:ro \
  -v "$A10/build":/dist \
  -e HOST_IDS="$(id -u):$(id -g)" \
  "alpine:$ALPINE_VER" sh -euc "
    mkdir /r
    apk add -q --root /r --initdb --keys-dir /etc/apk/keys \
        --repositories-file /overlay/etc/apk/repositories $PACKAGES
    cp -a /overlay/. /r/
    sed -i 's/^root:[^:]*:/root::/' /r/etc/shadow
    chmod 755 /r/sbin/a10-init
    mkdir -p /r/root/.ssh /r/etc/dropbear
    chmod 700 /r/root /r/root/.ssh
    tar -C /r -czf /dist/alpine-a10.tar.gz .
    chown \$HOST_IDS /dist/alpine-a10.tar.gz
  "
ls -la "$A10/build/alpine-a10.tar.gz"
