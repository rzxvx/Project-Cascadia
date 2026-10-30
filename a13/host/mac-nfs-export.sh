#!/usr/bin/env bash
#
# Put the iPhone's root file system (Alpine) on this Mac and export it over the
# USB network.  Needs sudo (nfsd, /etc/exports, root-owned files).
#
#   a13/host/mac-nfs-export.sh on       # unpack the rootfs once, export it
#   a13/host/mac-nfs-export.sh off      # stop exporting (the files stay)
#   a13/host/mac-nfs-export.sh status
#
# Then pack and boot as usual: boot/package.sh picks the export up from
# a13/build/nfsroot and the initramfs switches to it (or stays in RAM if the
# mount fails).
#
# NFSv3: macOS's nfsd serves v2/v3 only.  The export lives in ~/cascadia-a13-root,
# not under Desktop/Documents/Downloads: those are TCC-protected and nfsd cannot
# read them.
set -euo pipefail

A13="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TARBALL="${TARBALL:-$A13/build/alpine-a13.tar.gz}"   # rootfs/build-alpine-rootfs.sh
DST="${DST:-$HOME/cascadia-a13-root}"
LAN_NET="${LAN_NET:-172.16.42.0}"
LAN_MASK="${LAN_MASK:-255.255.255.0}"
IP_HOST="${IP_HOST:-172.16.42.2}"                   # the Mac, as the phone's udhcpd hands it out

# Our /etc/exports line is identified by the exported path; macOS exports(5)
# has no trailing comments, so there is no marker.
ours() { awk -v p="$DST " 'index($0, p) != 1' "$1" 2>/dev/null || true; }

[ "$(uname -s)" = "Darwin" ] || { echo "this one runs on the Mac" >&2; exit 1; }
fail() { echo "error: $*" >&2; exit 1; }

case "${1:-}" in
on)
    # Once the phone has booted from $DST it IS the live root: every `apk add`
    # lands there.  So unpack once, and refuse afterwards unless asked.
    if [ -d "$DST" ] && [ -n "$(ls -A "$DST" 2>/dev/null)" ] && [ "${FORCE_SYNC:-0}" != "1" ]; then
        echo "==> $DST already populated -- NOT overwriting it (FORCE_SYNC=1 to reseed)"
    else
        [ -f "$TARBALL" ] || fail "no $TARBALL -- run a13/rootfs/build-alpine-rootfs.sh first"
        echo "==> unpacking $TARBALL -> $DST"
        sudo rm -rf "$DST"
        sudo mkdir -p "$DST"
        sudo tar -xpzf "$TARBALL" -C "$DST"
        sudo mkdir -p "$DST/dev" "$DST/proc" "$DST/sys"
    fi

    echo "==> exporting $DST to $LAN_NET/$LAN_MASK"
    sudo touch /etc/exports
    sudo cp /etc/exports "/etc/exports.cascadia-backup.$(date +%s)"
    TMP=$(mktemp)
    ours /etc/exports > "$TMP"
    echo "$DST -maproot=root:wheel -network $LAN_NET -mask $LAN_MASK" >> "$TMP"
    sudo cp "$TMP" /etc/exports; rm -f "$TMP"
    sudo nfsd enable >/dev/null 2>&1 || true
    sudo nfsd start  >/dev/null 2>&1 || true
    sudo nfsd update >/dev/null 2>&1 || true
    sleep 1
    sudo nfsd checkexports || fail "nfsd rejected /etc/exports"
    showmount -e localhost || true

    echo "$IP_HOST:$DST" > "$A13/build/nfsroot"
    echo
    echo "==> NFS root = $IP_HOST:$DST (in a13/build/nfsroot)"
    echo "    Now: sh a13/boot/package.sh && boot as usual"
    ;;
off)
    TMP=$(mktemp)
    ours /etc/exports > "$TMP"
    sudo cp "$TMP" /etc/exports; rm -f "$TMP"
    sudo nfsd update >/dev/null 2>&1 || true
    rm -f "$A13/build/nfsroot"
    echo "==> export removed; $DST left on disk. Re-pack to go back to the RAM root."
    ;;
status)
    echo "--- /etc/exports (ours) ---"
    awk -v p="$DST " 'index($0, p) == 1' /etc/exports 2>/dev/null | grep . || echo "(none)"
    echo "--- exported now ---"; showmount -e localhost 2>&1 | head -5 || true
    echo "--- a13/build/nfsroot ---"; cat "$A13/build/nfsroot" 2>/dev/null || echo "(none: RAM root)"
    ;;
*)
    sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
