#!/usr/bin/env bash
set -euo pipefail
A12="$(cd "$(dirname "$0")/.." && pwd)"
TARBALL="${TARBALL:-$A12/build/debian-xr.tar.gz}"
DST="${DST:-/srv/cascadia-xr-debian}"
LAN="${LAN:-172.16.42.0/24}"
IP_HOST="${IP_HOST:-172.16.42.2}"
EXPORTS=/etc/exports.d/cascadia-xr-debian.exports
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
    echo "$IP_HOST:$DST" > "$A12/build/nfsroot"
    ;;
off)
    rm -f "$EXPORTS" "$A12/build/nfsroot"
    exportfs -ra
    ;;
status)
    cat "$EXPORTS" 2>/dev/null || true
    exportfs -v 2>/dev/null | grep -F "$DST" || true
    cat "$A12/build/nfsroot" 2>/dev/null || true
    ;;
*) echo "usage: $0 on|off|status" >&2; exit 2 ;;
esac
