#!/usr/bin/env bash
# build-alpine-rootfs.sh - Alpine Linux (aarch64) root file system for N104AP,
# to be served over NFS by host/mac-nfs-export.sh.
#
# Built with apk in an arm64 Alpine container and packed as a tarball, so
# owners, modes and setuid bits survive (the host extracts it as root).
# BusyBox init with our own inittab (overlay/), not OpenRC: the initramfs has
# already done the boot; a13-init only starts the services.
#
# Output: a13/build/alpine-a13.tar.gz
set -euo pipefail

ALPINE_VER="${ALPINE_VER:-3.24}"
PACKAGES="alpine-base dropbear dropbear-scp openssh-sftp-server htop nano eudev"
A13="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$A13/build"

docker run --rm --platform linux/arm64 \
  -v "$A13/rootfs/overlay":/overlay:ro \
  -v "$A13/build":/dist \
  "alpine:$ALPINE_VER" sh -euxc "
    mkdir /r
    apk add --root /r --initdb --keys-dir /etc/apk/keys \
        --repositories-file /overlay/etc/apk/repositories $PACKAGES
    cp -a /overlay/. /r/
    # root logs in with a key only (ssh) or through the serial autologin
    sed -i "s/^root::/root:*:/" /r/etc/shadow
    chmod 755 /r/sbin/a13-init
    mkdir -p /r/root/.ssh /r/etc/dropbear
    chmod 700 /r/root /r/root/.ssh
    tar -C /r -czf /dist/alpine-a13.tar.gz .
  "
ls -la "$A13/build/alpine-a13.tar.gz"
