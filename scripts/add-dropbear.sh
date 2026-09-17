#!/usr/bin/env bash
#
# Put an SSH server into build/initramfs-root and authorise this Mac's key.
#
# The unpacking is scripts/add-apk-packages.sh; this only adds the part that is
# specific to dropbear, which is the key.  dropbear has to be baked in rather
# than installed with apk on the device, because until there is an SSH server
# there is no shell over the network, and until there is a shell over the
# network there is no convenient way to run apk.
#
#   bash scripts/add-dropbear.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TREE="$ROOT/build/initramfs-root"
PKGS="${PKGS:-dropbear dropbear-scp}"

fail() { echo "error: $*" >&2; exit 1; }

# Find the key BEFORE spending a network round-trip.  dropbear refuses root on a
# blank password, and the Alpine overlay blanks root's password on purpose so
# the framebuffer console needs no typing.  A public key rather than a password
# means nothing secret lands in this repo or in the initramfs, and it still
# holds up once this box is on WiFi and stops being reachable only over a
# point-to-point cable.
PUB="${PUBKEY:-}"
if [ -z "$PUB" ]; then
    for c in ~/.ssh/id_ed25519.pub ~/.ssh/id_rsa.pub ~/.ssh/id_ecdsa.pub; do
        if [ -f "$c" ]; then PUB="$c"; break; fi
    done
fi
if [ -z "$PUB" ]; then
    echo "No SSH public key found in ~/.ssh." >&2
    echo >&2
    echo "  Make one:  ssh-keygen -t ed25519" >&2
    echo "  Or:        PUBKEY=~/.ssh/whatever.pub bash scripts/add-dropbear.sh" >&2
    echo >&2
    echo "Not configuring a password or a blank-password server -- this box" >&2
    echo "gets WiFi eventually." >&2
    exit 1
fi
[ -f "$PUB" ] || fail "no such public key: $PUB"
case "$(cat "$PUB")" in
    ssh-*|ecdsa-*|sk-*) : ;;
    *) fail "$PUB does not look like an OpenSSH public key -- pointed at a PRIVATE key by mistake?" ;;
esac
echo "==> will install $PUB as root's authorized_keys"

# shellcheck disable=SC2086
bash "$ROOT/scripts/add-apk-packages.sh" $PKGS

[ -x "$TREE/usr/sbin/dropbear" ] || fail "dropbear did not land in $TREE/usr/sbin"

mkdir -p "$TREE/root/.ssh"
cp "$PUB" "$TREE/root/.ssh/authorized_keys"
chmod 700 "$TREE/root/.ssh"
chmod 600 "$TREE/root/.ssh/authorized_keys"
echo "==> installed $PUB as root's authorized_keys"
echo
echo "Rebuild to embed it:   ./tools/rebuild-rearm.sh"
echo "Then, after flashing:  ssh root@10.55.0.2"
