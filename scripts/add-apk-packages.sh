#!/usr/bin/env bash
#
# Bake Alpine packages into build/initramfs-root, resolving shared-library
# dependencies, without running apk.
#
#   bash scripts/add-apk-packages.sh nfs-utils
#
# apk itself now works ON THE DEVICE -- it is an armhf binary and the device is
# an armv7 machine with a network -- so this is NOT the way to install software
# in general.  Use `apk add` over ssh for that; it is a real package manager and
# this is not.
#
# This is for the narrow case apk cannot serve: something that has to be present
# BEFORE first boot, when there is no network yet.  dropbear was one (no shell
# over the network without it, and no network without a shell).  mount.nfs is
# another: the root filesystem cannot be mounted by a binary that lives on the
# root filesystem.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TREE="$ROOT/build/initramfs-root"
WORK="build/apk-unpack"                    # relative to ROOT, i.e. /ibss in the container
IMAGE="${IMAGE:-ipad-mini-linux}"

ALPINE_VER="${ALPINE_VER:-3.24}"
ALPINE_ARCH="${ALPINE_ARCH:-armhf}"
MIRROR="${MIRROR:-https://dl-cdn.alpinelinux.org}"
REPO="$MIRROR/alpine/v$ALPINE_VER/main/$ALPINE_ARCH"

fail() { echo "error: $*" >&2; exit 1; }
[ $# -gt 0 ] || fail "usage: $0 <package>..."

[ -d "$TREE" ] || fail "no rootfs at $TREE -- run scripts/build-alpine-rootfs.sh first"
grep -qi '^ID=alpine' "$TREE/etc/os-release" 2>/dev/null \
    || fail "$TREE does not look like an Alpine rootfs"
[ "$(cat "$TREE/etc/apk/arch" 2>/dev/null)" = "$ALPINE_ARCH" ] \
    || fail "rootfs arch is not $ALPINE_ARCH -- set ALPINE_ARCH to match"

# In the build image, not on macOS: it needs network, python3 and readelf, and
# this is where the rest of the build already happens.
docker run --rm -v "$ROOT":/ibss "$IMAGE" \
    python3 /ibss/scripts/apk-unpack.py \
        --repo "$REPO" --dest "/ibss/$WORK" --rootfs /ibss/build/initramfs-root \
        "$@"

EX="$ROOT/$WORK"

# Nothing target-arch is ever executed here, so confirm by inspection that what
# came down is really 32-bit ARM: a wrong mirror path would otherwise install
# binaries that only fail on the device.
checked=0
while IFS= read -r f; do
    case "$(file -b "$f")" in
        *"ELF 32-bit"*ARM*) checked=$((checked + 1)) ;;
        *"ELF"*) fail "$f is not a 32-bit ARM binary: $(file -b "$f")" ;;
    esac
done < <(find "$EX" -type f)
[ "$checked" -gt 0 ] || fail "no ELF binaries came down at all -- nothing was installed"
echo "==> ok: $checked ARM binaries"

echo "==> installing into $TREE"
( cd "$EX" && find . \( -type f -o -type l \) -print | while read -r f; do
    mkdir -p "$TREE/$(dirname "$f")"
    cp -a "$f" "$TREE/$f"
done )
echo "==> done.  Rebuild to embed:  ./tools/rebuild-rearm.sh"
