#!/usr/bin/env bash
#
# Build Alpine armhf minirootfs for CONFIG_INITRAMFS_SOURCE.
#
# Downloads the official alpine-minirootfs tarball, extracts it, lays the
# repository's two overlays on top -- rootfs/alpine/ for the Alpine-side
# configuration and initramfs/ for this port's own boot scripts -- adds the
# packages that cannot be installed after first boot, and installs the result
# into build/initramfs-root, which is what build-kernel.sh embeds.
#
# The second overlay is the point.  For a while the boot scripts existed only
# inside build/initramfs-root, edited in place, while rootfs/alpine/init still
# held a copy from before USB networking existed.  A clean clone therefore
# built a kernel that came up on the glass with no console over the cable, no
# 10.55.0.2 and no ssh -- and nothing said so, because everything that checks
# the build checks the kernel.  initramfs/ is now the source of truth and this
# script is the only thing that assembles the tree.
#
# Host deps: wget or curl, tar, cpio, gzip.  No qemu.  Docker is needed only
# for the package step, which is skipped with a warning when it is missing.
#
# Usage:
#   bash scripts/build-alpine-rootfs.sh
#   bash scripts/build-kernel.sh
#
# Minimal busybox fallback: FORCE_MINIMAL=1 bash scripts/build-initramfs.sh
#
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ALPINE_VER="${ALPINE_VER:-3.24.1}"
ALPINE_ARCH="${ALPINE_ARCH:-armhf}"
ALPINE_MIRROR="${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org}"
TARBALL="alpine-minirootfs-${ALPINE_VER}-${ALPINE_ARCH}.tar.gz"
URL="${ALPINE_MIRROR}/alpine/v${ALPINE_VER%.*}/releases/${ALPINE_ARCH}/${TARBALL}"

CACHE="$ROOT/build/cache"
ALPINE_TREE="$ROOT/build/alpine-rootfs"
OUTDIR="$ROOT/build/initramfs-root"
OUT_CPIO="$ROOT/build/out/initramfs.cpio.gz"
OVERLAY="$ROOT/rootfs/alpine"
BOOT="$ROOT/initramfs"
STAMP="$ALPINE_TREE/.alpine-build-stamp"

mkdir -p "$CACHE" "$ROOT/build/out"

need_cmd() {
	if ! command -v "$1" >/dev/null 2>&1; then
		echo "missing required command: $1" >&2
		exit 1
	fi
}

need_cmd tar
need_cmd cpio
need_cmd gzip

fetch() {
	local dest="$1" url="$2"
	if command -v curl >/dev/null 2>&1; then
		curl -fL --connect-timeout 30 --retry 3 -o "$dest" "$url"
	elif command -v wget >/dev/null 2>&1; then
		wget -O "$dest" --timeout=30 --tries=3 "$url"
	else
		echo "need curl or wget to download $url" >&2
		exit 1
	fi
}

echo "==> Alpine minirootfs ${ALPINE_VER} (${ALPINE_ARCH})"
echo "    URL: $URL"

TARPATH="$CACHE/$TARBALL"
if [ ! -f "$TARPATH" ]; then
	echo "==> downloading $TARBALL"
	fetch "$TARPATH.partial" "$URL"
	mv "$TARPATH.partial" "$TARPATH"
else
	echo "==> using cached $TARPATH"
fi

echo "==> extracting into $ALPINE_TREE"
rm -rf "$ALPINE_TREE"
mkdir -p "$ALPINE_TREE"
tar -xzf "$TARPATH" -C "$ALPINE_TREE"

echo "==> applying overlay from $OVERLAY"
if [ ! -d "$OVERLAY" ]; then
	echo "overlay missing: $OVERLAY" >&2
	exit 1
fi
# Overlay may include dirs that already exist; -a preserves modes where possible.
cp -a "$OVERLAY"/. "$ALPINE_TREE"/

echo "==> applying boot scripts from $BOOT"
for f in init sbin/p105-stage2; do
	if [ ! -f "$BOOT/$f" ]; then
		echo "missing $BOOT/$f -- initramfs/ is the source of truth for the boot scripts" >&2
		exit 1
	fi
done
cp -a "$BOOT"/. "$ALPINE_TREE"/
chmod 755 "$ALPINE_TREE/init" "$ALPINE_TREE/sbin/p105-stage2"

# Ensure apk repos dir exists even if overlay copy was partial.
mkdir -p "$ALPINE_TREE/etc/apk"
if [ ! -f "$ALPINE_TREE/etc/apk/repositories" ]; then
	echo "overlay did not install etc/apk/repositories" >&2
	exit 1
fi

# Empty root password for glass getty (embedded bring-up only).
#
# Not `sed -i`: BSD sed requires an argument to it and GNU sed refuses one, so
# there is no spelling that works on both.  This script had never actually run
# on the Mac until now -- the tree used to be built in a Linux container and
# copied -- and `sed -i 's|...|' file` on macOS reads the expression as the
# backup suffix and the path as the script, which fails as "invalid command
# code k", k being the first letter of the user's home directory.
#
# `cat >` rather than `mv`: it truncates the file in place and keeps its mode.
if [ -f "$ALPINE_TREE/etc/shadow" ]; then
	sed 's|^root:[^:]*:|root::|' "$ALPINE_TREE/etc/shadow" > "$ALPINE_TREE/etc/shadow.new"
	cat "$ALPINE_TREE/etc/shadow.new" > "$ALPINE_TREE/etc/shadow"
	rm -f "$ALPINE_TREE/etc/shadow.new"
fi

# Static /dev nodes so the kernel can open an initial console before /init.
# (Avoids "Warning: unable to open an initial console.") Needs mknod capability.
echo "==> seeding /dev console nodes"
mkdir -p "$ALPINE_TREE/dev"
seed_node() {
	local path="$1" mode="$2" type="$3" maj="$4" min="$5"
	if [ -e "$path" ]; then
		return 0
	fi
	if mknod -m "$mode" "$path" "$type" "$maj" "$min" 2>/dev/null; then
		return 0
	fi
	# Retry via fakeroot-less python if available (still needs CAP_MKNOD).
	return 1
}
CONSOLE_OK=1
seed_node "$ALPINE_TREE/dev/console" 600 c 5 1 || CONSOLE_OK=0
seed_node "$ALPINE_TREE/dev/tty" 666 c 5 0 || true
seed_node "$ALPINE_TREE/dev/tty0" 620 c 4 0 || CONSOLE_OK=0
seed_node "$ALPINE_TREE/dev/tty1" 620 c 4 1 || true
seed_node "$ALPINE_TREE/dev/null" 666 c 1 3 || true
seed_node "$ALPINE_TREE/dev/zero" 666 c 1 5 || true
if [ "$CONSOLE_OK" != 1 ]; then
	echo "    note: mknod needs CAP_MKNOD and this is not root -- harmless."
	echo "          CONFIG_DEVTMPFS_MOUNT=y, so the kernel mounts devtmpfs on"
	echo "          /dev before init runs and every node appears by itself."
fi

{
	echo "alpine-minirootfs ${ALPINE_VER}-${ALPINE_ARCH}"
	echo "source=$URL"
	echo "built=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} >"$STAMP"

echo "==> installing into $OUTDIR (CONFIG_INITRAMFS_SOURCE)"
rm -rf "$OUTDIR"
mkdir -p "$(dirname "$OUTDIR")"
cp -a "$ALPINE_TREE" "$OUTDIR"
# Drop stamp from the embedded tree (optional clutter).
rm -f "$OUTDIR/.alpine-build-stamp"

# ------------------------------------------------------------------ packages --
# Two things have to be in the image before the first boot, because neither can
# be installed once it is running: dropbear, because there is no shell over the
# network without it and no convenient apk without that shell, and mount.nfs,
# because the root filesystem cannot be mounted by a binary that lives on the
# root filesystem.  Everything else is `apk add` on the device.
#
# Unpacking them needs the build image (readelf and a network), so a machine
# without a running docker gets a warning rather than a failure: the tree still
# boots, and still gives a console on the glass and over the cable.
if [ "${SKIP_PACKAGES:-0}" = 1 ]; then
	echo "==> SKIP_PACKAGES=1 -- no dropbear, no mount.nfs"
elif docker info >/dev/null 2>&1; then
	bash "$ROOT/scripts/add-apk-packages.sh" nfs-utils
	PUBKEY_OPTIONAL=1 bash "$ROOT/scripts/add-dropbear.sh"
else
	echo "==> docker is not reachable -- skipping dropbear and nfs-utils."
	echo "    The tree boots without them, but there is no ssh and no NFS root."
	echo "    Once docker runs:"
	echo "      bash scripts/add-dropbear.sh"
	echo "      bash scripts/add-apk-packages.sh nfs-utils"
fi

# Anything left in build/keep/ is copied in last.  It is for files that are not
# yet reproducible from this repository -- hx-touchd, the Sandcastle touch
# daemon, is the current one -- so that rebuilding the rootfs does not quietly
# drop them on the floor.
if [ -d "$ROOT/build/keep" ]; then
	echo "==> copying build/keep/ over the tree"
	cp -a "$ROOT/build/keep/." "$OUTDIR/"
fi

echo "==> packing $OUT_CPIO"
(
	cd "$OUTDIR"
	find . | cpio -o -H newc --owner 0:0 2>/dev/null
) | gzip -9 >"$OUT_CPIO"

SIZE_TREE=$(du -sh "$OUTDIR" | awk '{print $1}')
SIZE_CPIO=$(ls -lh "$OUT_CPIO" | awk '{print $5}')
echo "    tree:  $OUTDIR ($SIZE_TREE)"
echo "    cpio:  $OUT_CPIO ($SIZE_CPIO)"
echo
echo "Next:  ./cascadia build"
echo
echo "Minimal busybox instead: FORCE_MINIMAL=1 bash scripts/build-initramfs.sh"
