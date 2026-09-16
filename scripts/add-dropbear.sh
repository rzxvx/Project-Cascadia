#!/usr/bin/env bash
#
# Install dropbear (SSH server + scp) into build/initramfs-root.
#
# Why this is a separate manual step and not part of build-alpine-rootfs.sh:
# the rootfs is Alpine armhf, and an Apple Silicon host cannot execute AArch32
# code at all -- there is no 32-bit EL0 on M-series.  So `apk add` can never be
# run against this tree from here.  An .apk is just concatenated gzip streams of
# tar archives, so we unpack it by hand instead, which needs no target-arch
# execution whatsoever.
#
#   bash scripts/add-dropbear.sh
#
# Run it once; the files then live in build/initramfs-root and get embedded in
# every subsequent kernel build via CONFIG_INITRAMFS_SOURCE.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TREE="$ROOT/build/initramfs-root"
WORK="$ROOT/build/dropbear-apk"
IMAGE="${IMAGE:-ipad-mini-linux}"

ALPINE_VER="${ALPINE_VER:-3.24}"
ALPINE_ARCH="${ALPINE_ARCH:-armhf}"
MIRROR="${MIRROR:-https://dl-cdn.alpinelinux.org}"
REPO="$MIRROR/alpine/v$ALPINE_VER/main/$ALPINE_ARCH"

fail() { echo "error: $*" >&2; exit 1; }

[ -d "$TREE" ] || fail "no rootfs at $TREE -- run scripts/build-alpine-rootfs.sh first"
grep -qi '^ID=alpine' "$TREE/etc/os-release" 2>/dev/null \
    || fail "$TREE does not look like an Alpine rootfs"
[ "$(cat "$TREE/etc/apk/arch" 2>/dev/null)" = "$ALPINE_ARCH" ] \
    || fail "rootfs arch is not $ALPINE_ARCH -- set ALPINE_ARCH to match"

mkdir -p "$WORK"

# Everything below runs inside the build image rather than on macOS: GNU tar
# follows the concatenated gzip streams in an .apk, bsdtar's handling of them is
# version-dependent, and this way the download happens in the same place the
# rest of the build already does.
echo "==> fetching dropbear + scp for Alpine v$ALPINE_VER/$ALPINE_ARCH"
docker run --rm -v "$ROOT":/ibss "$IMAGE" bash -c "
set -euo pipefail
cd /ibss/build/dropbear-apk
rm -rf extract; mkdir -p extract

index=\$(curl -fsSL '$REPO/') || { echo 'cannot reach $REPO'; exit 1; }
for pkg in dropbear dropbear-scp; do
    # \\b-style anchor: 'dropbear-' would also match 'dropbear-scp-...', so pin
    # the character right after the package name to a digit.
    file=\$(printf '%s' \"\$index\" | grep -oE \"\$pkg-[0-9][^\\\"]*\\.apk\" | sort -u | head -1)
    [ -n \"\$file\" ] || { echo \"no \$pkg apk found in the index\"; exit 1; }
    echo \"    \$file\"
    curl -fsSL -o \"\$file\" \"$REPO/\$file\"
    tar -xzf \"\$file\" -C extract 2>/dev/null || true
done

# Package metadata is not part of the filesystem.
rm -f extract/.PKGINFO extract/.SIGN.* extract/.pre-install extract/.post-install
find extract -type f | sed 's|^extract/|    |' | sort
"

EX="$WORK/extract"
[ -x "$EX/usr/sbin/dropbear" ] || fail "dropbear binary did not land in $EX/usr/sbin"

# The whole point of unpacking by hand is that nothing target-arch ever runs, so
# verify by inspection that what we got is actually 32-bit ARM and not, say, the
# x86_64 package because a mirror path was wrong.
case "$(file -b "$EX/usr/sbin/dropbear")" in
    *"ELF 32-bit"*ARM*) : ;;
    *) fail "dropbear is not a 32-bit ARM binary: $(file -b "$EX/usr/sbin/dropbear")" ;;
esac
echo "    ok: dropbear is 32-bit ARM"

echo "==> installing into $TREE"
( cd "$EX" && find . -type f -o -type l | while read -r f; do
    mkdir -p "$TREE/$(dirname "$f")"
    cp -a "$f" "$TREE/$f"
done )

# ---------------------------------------------------------------- auth ------
# Dropbear refuses to let root in on a blank password, and the Alpine overlay
# blanks root's password on purpose so the framebuffer console needs no typing.
# So give it something real.  A public key is preferred: nothing secret ends up
# in this repo or in the initramfs, and it survives WiFi later, when this box
# stops being reachable only over a point-to-point cable.
mkdir -p "$TREE/root/.ssh"
PUB=""
for c in ~/.ssh/id_ed25519.pub ~/.ssh/id_rsa.pub ~/.ssh/id_ecdsa.pub; do
    [ -f "$c" ] && { PUB="$c"; break; }
done

if [ -n "$PUB" ]; then
    cp "$PUB" "$TREE/root/.ssh/authorized_keys"
    chmod 700 "$TREE/root/.ssh"
    chmod 600 "$TREE/root/.ssh/authorized_keys"
    echo "    installed $PUB as root's authorized_keys"
    echo "    login:  ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@10.55.0.2"
else
    echo
    echo "    No SSH public key found in ~/.ssh."
    echo "    Make one with:  ssh-keygen -t ed25519"
    echo "    then re-run this script.  Refusing to configure a password or a"
    echo "    blank-password server -- this box gets WiFi eventually."
    fail "no public key to install"
fi

echo
echo "==> done.  Rebuild to embed it:  ./tools/rebuild-rearm.sh"
