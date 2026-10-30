#!/usr/bin/env bash
#
# Put the iPhone's root file system (Alpine) on this Linux host and export it
# over the USB network.  The Linux twin of mac-nfs-export.sh; a13/cascadia nfs
# picks by host.  Needs sudo and nfs-utils.
#
#   a13/host/linux-nfs-export.sh on|off|status
#
# NFSv3 with no_root_squash: the initramfs mounts with vers=3 (the macOS host
# has no v4) and the phone runs as root.  The export lives in /srv, not under
# $HOME: a 0700 home is not traversable by nfsd.
set -euo pipefail

A13="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARBALL="${TARBALL:-$A13/build/alpine-a13.tar.gz}"   # rootfs/build-alpine-rootfs.sh
DST="${DST:-/srv/cascadia-a13-root}"
LAN="${LAN:-172.16.42.0/24}"
IP_HOST="${IP_HOST:-172.16.42.2}"                   # this host, as the phone's udhcpd hands it out
EXPORTS=/etc/exports.d/cascadia-a13.exports

[ "$(uname -s)" = "Linux" ] || { echo "this one runs on a Linux host; on macOS it is mac-nfs-export.sh" >&2; exit 1; }
fail() { echo "error: $*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

firewall_note() {
    local f=""
    if systemctl is-active --quiet firewalld 2>/dev/null; then f=firewalld; fi
    if grep -qs '^ENABLED=yes' /etc/ufw/ufw.conf; then f=ufw; fi
    [ -n "$f" ] || return 0
    echo "note: $f is running.  The phone needs TCP 111 (rpcbind), 2049 (nfsd) and"
    echo "      mountd's port from $LAN; if the mount times out at boot, allow them there."
}

case "${1:-}" in
on)
    have exportfs || fail "no exportfs -- install nfs-utils (Arch: sudo pacman -S nfs-utils)"
    # Once the phone has booted from $DST it IS the live root: every `apk add`
    # lands there.  So unpack once, and refuse afterwards unless asked.
    if [ -d "$DST" ] && [ -n "$(ls -A "$DST" 2>/dev/null)" ] && [ "${FORCE_SYNC:-0}" != "1" ]; then
        echo "==> $DST already populated -- NOT overwriting it (FORCE_SYNC=1 to reseed)"
    else
        [ -f "$TARBALL" ] || fail "no $TARBALL -- run: a13/cascadia build rootfs"
        echo "==> unpacking $TARBALL -> $DST"
        sudo rm -rf "$DST"
        sudo mkdir -p "$DST"
        sudo tar -xpzf "$TARBALL" -C "$DST"
        sudo mkdir -p "$DST/dev" "$DST/proc" "$DST/sys"
    fi

    echo "==> exporting $DST to $LAN"
    sudo mkdir -p /etc/exports.d
    echo "$DST $LAN(rw,sync,no_subtree_check,no_root_squash,insecure)" | sudo tee "$EXPORTS" >/dev/null
    sudo systemctl enable --now rpcbind.socket nfs-server.service
    sudo exportfs -ra
    sudo exportfs -v | grep -F "$DST" >/dev/null \
        || fail "exportfs does not list $DST -- see: sudo exportfs -v; journalctl -u nfs-server"
    if have rpcinfo && ! rpcinfo -p localhost 2>/dev/null | awk '$5 == "nfs" && $2 == 3' | grep -q .; then
        echo "warning: nfsd does not offer v3 -- set vers3=y under [nfsd] in /etc/nfs.conf" >&2
    fi
    firewall_note

    mkdir -p "$A13/build"
    echo "$IP_HOST:$DST" > "$A13/build/nfsroot"
    echo
    echo "==> NFS root = $IP_HOST:$DST (in a13/build/nfsroot)"
    echo "    Now: a13/cascadia pack && a13/cascadia boot"
    ;;
off)
    sudo rm -f "$EXPORTS"
    if have exportfs; then sudo exportfs -ra; fi
    rm -f "$A13/build/nfsroot"
    echo "==> export removed; $DST left on disk. Re-pack to go back to the RAM root."
    ;;
status)
    echo "--- $EXPORTS ---"; cat "$EXPORTS" 2>/dev/null || echo "(none)"
    echo "--- nfs-server ---"; systemctl is-active nfs-server.service 2>&1 || true
    echo "--- exported now ---"; sudo exportfs -v 2>&1 | grep -F -A1 "$DST" || echo "(not exported)"
    echo "--- a13/build/nfsroot ---"; cat "$A13/build/nfsroot" 2>/dev/null || echo "(none: RAM root)"
    ;;
*)
    sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
