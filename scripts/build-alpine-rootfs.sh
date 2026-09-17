#!/usr/bin/env bash
#
# Build Alpine armhf minirootfs for CONFIG_INITRAMFS_SOURCE.
#
# Downloads the official alpine-minirootfs tarball, extracts it, applies
# rootfs/alpine/ overlay (tty0 getty, apk repos, motd), and installs the
# tree into build/initramfs-root so the existing build-kernel.sh path is
# unchanged. Also keeps a copy under build/alpine-rootfs and packs
# build/out/initramfs.cpio.gz for inspection.
#
# Host deps: wget or curl, tar, cpio, gzip. No qemu.
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
chmod 755 "$ALPINE_TREE/init"

# Ensure apk repos dir exists even if overlay copy was partial.
mkdir -p "$ALPINE_TREE/etc/apk"
if [ ! -f "$ALPINE_TREE/etc/apk/repositories" ]; then
	echo "overlay did not install etc/apk/repositories" >&2
	exit 1
fi

# Empty root password for glass getty (embedded bring-up only).
if [ -f "$ALPINE_TREE/etc/shadow" ]; then
	sed -i 's|^root:[^:]*:|root::|' "$ALPINE_TREE/etc/shadow"
fi

# Bring-up busybox with cttyhack (Alpine's busybox often lacks that applet).
BB_HOST="$ROOT/build/out/busybox-armhf"
if [ -x "$BB_HOST" ]; then
	cp "$BB_HOST" "$ALPINE_TREE/bin/busybox-p105"
	chmod 755 "$ALPINE_TREE/bin/busybox-p105"
	echo "==> installed /bin/busybox-p105 (cttyhack fallback) from $BB_HOST"
else
	echo "==> note: no $BB_HOST — getty -n -l will be the console path"
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
