#!/usr/bin/env bash
#
# Install dropbear (SSH server) and scp into build/initramfs-root.
#
# Why this is a separate manual step and not part of build-alpine-rootfs.sh:
# the rootfs is Alpine armhf and an Apple Silicon host cannot execute AArch32
# code at all -- there is no 32-bit EL0 on M-series -- so `apk add` can never be
# run against this tree from here.  scripts/apk-unpack.py does the equivalent
# with pure data handling instead; see its docstring for what it does and does
# not promise.
#
#   bash scripts/add-dropbear.sh
#
# Run once; the files then live in build/initramfs-root and get embedded in
# every subsequent kernel build via CONFIG_INITRAMFS_SOURCE.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TREE="$ROOT/build/initramfs-root"
WORK="build/dropbear-apk/extract"          # relative to ROOT, i.e. /ibss in the container
IMAGE="${IMAGE:-ipad-mini-linux}"

ALPINE_VER="${ALPINE_VER:-3.24}"
ALPINE_ARCH="${ALPINE_ARCH:-armhf}"
MIRROR="${MIRROR:-https://dl-cdn.alpinelinux.org}"
REPO="$MIRROR/alpine/v$ALPINE_VER/main/$ALPINE_ARCH"
PKGS="${PKGS:-dropbear dropbear-scp}"

fail() { echo "error: $*" >&2; exit 1; }

[ -d "$TREE" ] || fail "no rootfs at $TREE -- run scripts/build-alpine-rootfs.sh first"
grep -qi '^ID=alpine' "$TREE/etc/os-release" 2>/dev/null \
    || fail "$TREE does not look like an Alpine rootfs"
[ "$(cat "$TREE/etc/apk/arch" 2>/dev/null)" = "$ALPINE_ARCH" ] \
    || fail "rootfs arch is not $ALPINE_ARCH -- set ALPINE_ARCH to match"

# Runs in the build image, not on macOS: it needs network, python3 and readelf,
# and this is where the rest of the build already happens.
docker run --rm -v "$ROOT":/ibss "$IMAGE" \
    python3 /ibss/scripts/apk-unpack.py \
        --repo "$REPO" --dest "/ibss/$WORK" --rootfs /ibss/build/initramfs-root \
        $PKGS

EX="$ROOT/$WORK"
[ -x "$EX/usr/sbin/dropbear" ] || fail "dropbear binary did not land in $EX/usr/sbin"

# Nothing target-arch is ever executed here, so confirm by inspection that what
# came down is really 32-bit ARM -- a wrong mirror path would otherwise install
# an x86_64 binary that only fails on the device.
case "$(file -b "$EX/usr/sbin/dropbear")" in
    *"ELF 32-bit"*ARM*) : ;;
    *) fail "dropbear is not a 32-bit ARM binary: $(file -b "$EX/usr/sbin/dropbear")" ;;
esac
echo "==> ok: dropbear is 32-bit ARM"

echo "==> installing into $TREE"
( cd "$EX" && find . \( -type f -o -type l \) -print | while read -r f; do
    mkdir -p "$TREE/$(dirname "$f")"
    cp -a "$f" "$TREE/$f"
done )

# ---------------------------------------------------------------- auth ------
# Dropbear refuses root on a blank password, and the Alpine overlay blanks
# root's password on purpose so the framebuffer console needs no typing.  Give
# it a public key rather than a password: nothing secret lands in this repo or
# in the initramfs, and it still holds up once this box is on WiFi and stops
# being reachable only over a point-to-point cable.
mkdir -p "$TREE/root/.ssh"
PUB=""
for c in ~/.ssh/id_ed25519.pub ~/.ssh/id_rsa.pub ~/.ssh/id_ecdsa.pub; do
    if [ -f "$c" ]; then PUB="$c"; break; fi
done

if [ -z "$PUB" ]; then
    echo
    echo "    No SSH public key found in ~/.ssh."
    echo "    Make one with:  ssh-keygen -t ed25519"
    echo "    then re-run this script.  Not configuring a password or a"
    echo "    blank-password server -- this box gets WiFi eventually."
    fail "no public key to install"
fi

cp "$PUB" "$TREE/root/.ssh/authorized_keys"
chmod 700 "$TREE/root/.ssh"
chmod 600 "$TREE/root/.ssh/authorized_keys"
echo "==> installed $PUB as root's authorized_keys"
echo
echo "Rebuild to embed it:   ./tools/rebuild-rearm.sh"
echo "Then, after flashing:  ssh -o StrictHostKeyChecking=no \\"
echo "                           -o UserKnownHostsFile=/dev/null root@10.55.0.2"
