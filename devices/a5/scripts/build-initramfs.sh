#!/usr/bin/env bash
#
# Build a minimal busybox rootfs for first Linux boot.
#
# Preferred path for on-device work: Alpine minirootfs via
#   bash scripts/build-alpine-rootfs.sh   (or: make alpine-rootfs)
# which installs into the same build/initramfs-root used by build-kernel.sh.
#
# This script remains the tiny busybox fallback. If an Alpine tree is already
# present in build/initramfs-root, it is left alone unless FORCE_MINIMAL=1.
#
# Outputs:
#   build/initramfs-root      the tree build-kernel.sh embeds via
#                             CONFIG_INITRAMFS_SOURCE (this is the one that boots)
#   build/out/initramfs.cpio.gz  the same tree packed, for an external initrd
#
set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/rootfs/minimal"
OUT="$ROOT/build/out/initramfs.cpio.gz"
BB="$ROOT/build/out/busybox-armhf"
OUTDIR="$ROOT/build/initramfs-root"

mkdir -p "$ROOT/build/out"

if ! command -v cpio >/dev/null 2>&1; then
    echo "cpio not found — install: sudo apt install cpio" >&2
    exit 1
fi

# Do not clobber Alpine unless explicitly requested.
if [ -f "$OUTDIR/etc/os-release" ] && grep -qi '^ID=alpine' "$OUTDIR/etc/os-release" 2>/dev/null; then
    if [ "${FORCE_MINIMAL:-0}" != "1" ]; then
        echo "==> Alpine rootfs already at $OUTDIR — keeping it"
        echo "    rebuild Alpine:  bash scripts/build-alpine-rootfs.sh"
        echo "    force minimal:   FORCE_MINIMAL=1 bash scripts/build-initramfs.sh"
        if [ ! -f "$OUT" ]; then
            echo "==> packing existing tree to $OUT"
            (
                cd "$OUTDIR"
                find . | cpio -o -H newc --owner 0:0 2>/dev/null
            ) | gzip -9 >"$OUT"
            ls -lh "$OUT"
        fi
        exit 0
    fi
    echo "==> FORCE_MINIMAL=1 — replacing Alpine with busybox minimal"
fi

echo "==> assembling initramfs tree"
rm -rf "$OUTDIR"
mkdir -p "$OUTDIR"/{bin,sbin,etc,proc,sys,dev,tmp,run}

cp "$SRC/init" "$OUTDIR/init"
chmod 755 "$OUTDIR/init"

if [ ! -x "$BB" ]; then
    cat >&2 <<EOF
no static ARM busybox at $BB

/init is a shell script, so without it the kernel reaches userspace and then
panics with "Failed to execute /init". Fetch one with:

  curl -fsSL -o $BB \\
    https://busybox.net/downloads/binaries/1.31.0-defconfig-multiarch-musl/busybox-armv7l
  chmod +x $BB
EOF
    exit 1
fi

if ! file "$BB" 2>/dev/null | grep -q ARM; then
    echo "$BB is not an ARM binary — it would not run on the iPad" >&2
    exit 1
fi

cp "$BB" "$OUTDIR/bin/busybox"
chmod 755 "$OUTDIR/bin/busybox"

# /init needs sh; the rest is what makes the shell usable enough to poke at
# hardware once we get there.
for app in sh ash mount umount ls cat echo dmesg mkdir ln rm ps free \
           setsid cttyhack sleep uname hexdump dd grep sed more clear reboot poweroff; do
    ln -sf busybox "$OUTDIR/bin/$app"
done
ln -sf ../bin/busybox "$OUTDIR/sbin/init"
echo "    busybox: $(stat -c%s "$BB") bytes, $(ls "$OUTDIR/bin" | wc -l) applets"

echo "==> packing cpio.gz"
(
    cd "$OUTDIR"
    find . | cpio -o -H newc --owner 0:0 2>/dev/null
) | gzip -9 > "$OUTDIR.cpio.gz"

mv "$OUTDIR.cpio.gz" "$OUT"
ls -lh "$OUT"
echo "tree for CONFIG_INITRAMFS_SOURCE: $OUTDIR"
