#!/usr/bin/env bash
set -euo pipefail
A12="$(cd "$(dirname "$0")/.." && pwd)"
source "$A12/rootfs/debian-profile.sh"
TARBALL="${TARBALL:-$A12/build/debian-$PROFILE.tar.gz}"
DST="${DST:-/srv/cascadia-$PROFILE-debian}"
LAN="${LAN:-172.16.42.0/24}"
IP_HOST="${IP_HOST:-172.16.42.2}"
EXPORTS="/etc/exports.d/cascadia-$PROFILE-debian.exports"
case "${1:-}" in
on)
    command -v exportfs >/dev/null || { echo 'install nfs-utils first' >&2; exit 1; }
    if [ -d "$DST" ] && [ -n "$(ls -A "$DST" 2>/dev/null)" ] && [ "${FORCE_SYNC:-0}" != 1 ]; then
        echo "$DST already populated; not overwriting"
    else
        test -s "$TARBALL"
        rm -rf "$DST"
        mkdir -p "$DST" "$DST/dev" "$DST/proc" "$DST/sys"
        tar -xpzf "$TARBALL" -C "$DST"
    fi
    # On reused roots the other board's firmware may be absent after switch_root.
    bash "$A12/rootfs/install-touch-firmware.sh" "$DST"
    mkdir -p /etc/exports.d
    echo "$DST $LAN(rw,sync,no_subtree_check,no_root_squash,insecure)" > "$EXPORTS"
    if [ -d /run/systemd/system ]; then
        systemctl enable --now rpcbind.socket nfs-server.service
    else
        mountpoint -q /proc/fs/nfsd || mount -t nfsd nfsd /proc/fs/nfsd
        pgrep -x rpcbind >/dev/null || rpcbind -w
        rpc.nfsd 8
        pgrep -x rpc.mountd >/dev/null || rpc.mountd
    fi
    exportfs -ra
    exportfs -v | grep -F "$DST"
    mkdir -p "$(dirname "$A12_NFSROOT")"
    echo "$IP_HOST:$DST" > "$A12_NFSROOT"
    ;;
off)
    rm -f "$EXPORTS" "$A12_NFSROOT"
    exportfs -ra
    ;;
status)
    cat "$EXPORTS" 2>/dev/null || true
    exportfs -v 2>/dev/null | grep -F "$DST" || true
    cat "$A12_NFSROOT" 2>/dev/null || true
    ;;
*) echo "usage: $0 on|off|status" >&2; exit 2 ;;
esac
