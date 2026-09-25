#!/usr/bin/env bash
#
# Put the iPad's root filesystem on this Linux host and export it over the ECM
# link.  The Linux twin of mac-nfs-export.sh; ./cascadia nfs picks by host.
#
#   ./tools/linux-nfs-export.sh on      # copy the rootfs, export it, point /init at it
#   ./tools/linux-nfs-export.sh off     # stop exporting, go back to the RAM rootfs
#   ./tools/linux-nfs-export.sh status
#
# The device mounts vers=3 over TCP with nolock (initramfs/init), so the server
# needs the v3 pieces: rpcbind for the handshake and mountd for the root file
# handle.  nfs-utils ships all of it (Arch: pacman -S nfs-utils); this starts
# rpcbind.socket and nfs-server.service and enables them for the next boot.
#
# The export lives in its own file, /etc/exports.d/cascadia.exports, so
# /etc/exports is never touched.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KEEP="$ROOT/build/keep"                    # survives a rootfs rebuild; the assembly
                                           # copies it back over the fresh tree
SRC="${SRC:-$ROOT/build/initramfs-root}"   # the tree ./cascadia rootfs builds and
                                           # ./cascadia build embeds
DST="${DST:-/srv/cascadia-root}"           # not under $HOME: a 0700 home, an
                                           # encrypted one or a systemd-homed one
                                           # is no place for nfsd
LAN="${LAN:-10.55.0.0/24}"
IP_HOST="${IP_HOST:-10.55.0.1}"
EXPORTS=/etc/exports.d/cascadia.exports

[ "$(uname -s)" = "Linux" ] || { echo "this one runs on a Linux host; on macOS it is mac-nfs-export.sh" >&2; exit 1; }
fail() { echo "error: $*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

# A firewall that filters input on the link turns the mount into a two-minute
# wait at boot and a RAM root afterwards, with nothing on the device to say
# why.  Not fixed from here -- only named, because a rule for it belongs in
# whatever manages that firewall.
firewall_note() {
    local f=""
    for s in firewalld ufw; do
        if systemctl is-active --quiet "$s" 2>/dev/null; then f="$s"; fi
    done
    [ -n "$f" ] || return 0
    echo "note: $f is running.  The device needs TCP 111 (rpcbind), 2049 (nfsd) and"
    echo "      mountd's port from $LAN; if the mount times out at boot, allow them there."
}

case "${1:-}" in
on)
    [ -d "$SRC" ] || fail "no rootfs at $SRC -- run ./cascadia rootfs first (or set SRC=)"
    [ -x "$SRC/sbin/p105-stage2" ] || fail "$SRC has no /sbin/p105-stage2 -- stage 1 would refuse it"
    [ -x "$SRC/sbin/mount.nfs" ] || echo "warning: no mount.nfs in the initramfs yet -- run: bash $ROOT/scripts/add-apk-packages.sh nfs-utils" >&2
    have exportfs || fail "no exportfs -- install nfs-utils (Arch: sudo pacman -S nfs-utils)"
    have rsync || fail "no rsync"

    # Same rule as on the Mac: once the device has booted from $DST, that
    # directory IS its live root, and an rsync --delete from the initramfs
    # tree would wipe everything installed there since.  Seed once.
    if [ -d "$DST" ] && [ -n "$(ls -A "$DST" 2>/dev/null)" ] && [ "${FORCE_SYNC:-0}" != "1" ]; then
        echo "==> $DST already populated -- NOT overwriting it"
        echo "    It is the device's live root now; anything installed there"
        echo "    would be deleted.  To reseed it from the initramfs anyway:"
        echo "      FORCE_SYNC=1 $0 on"
    else
        echo "==> copying $SRC -> $DST"
        sudo mkdir -p "$DST"
        # Device nodes and the kernel's filesystems are left out: stage 1 moves
        # devtmpfs, proc and sys onto the new root itself.
        sudo rsync -a --delete \
            --exclude 'dev/*' --exclude 'proc/*' --exclude 'sys/*' --exclude 'newroot' \
            "$SRC/" "$DST/"
        # The tree was built as this user.  The device writes as uid 0 through
        # no_root_squash, so the files it finds should be root's already.
        sudo chown -R root:root "$DST"
        sudo mkdir -p "$DST/dev" "$DST/proc" "$DST/sys"
    fi

    echo "==> exporting $DST to $LAN"
    # sync: the device has no way to learn that the host lost writes it was
    # told were on disk.  no_root_squash: the device runs everything as root.
    sudo mkdir -p /etc/exports.d
    echo "$DST $LAN(rw,sync,no_subtree_check,no_root_squash)" | sudo tee "$EXPORTS" >/dev/null
    sudo systemctl enable --now rpcbind.socket nfs-server.service
    sudo exportfs -ra
    sudo exportfs -v | grep -F "$DST" >/dev/null \
        || fail "exportfs does not list $DST -- see: sudo exportfs -v; journalctl -u nfs-server"
    # vers=3 is what the device asks for; a server set to v4 only would refuse
    # it at mount time, on the device, where nobody is looking.
    if have rpcinfo && ! rpcinfo -p localhost 2>/dev/null | awk '$5 == "nfs" && $2 == 3' | grep -q .; then
        echo "warning: nfsd does not offer v3 -- check vers3 in /etc/nfs.conf" >&2
    fi
    firewall_note

    # Stage 1 reads this at boot; absent, it stays in RAM.  Written into the
    # tree and into build/keep/, which survives ./cascadia rootfs.
    echo "$IP_HOST:$DST" > "$SRC/etc/nfsroot"
    mkdir -p "$KEEP/etc"
    echo "$IP_HOST:$DST" > "$KEEP/etc/nfsroot"
    echo
    echo "==> /etc/nfsroot in the initramfs = $IP_HOST:$DST"
    echo "    Rebuild and flash for it to take effect:"
    echo "      ./cascadia build && ./cascadia flash"
    ;;
off)
    sudo rm -f "$EXPORTS"
    if have exportfs; then sudo exportfs -ra; fi
    rm -f "$SRC/etc/nfsroot" "$KEEP/etc/nfsroot"
    echo "==> export removed and /etc/nfsroot cleared."
    echo "    nfs-server is left running for whatever else it serves;"
    echo "    $DST is left on disk; delete it yourself if you want it gone."
    echo "    Rebuild and flash to go back to the RAM rootfs."
    ;;
status)
    echo "--- $EXPORTS ---"; cat "$EXPORTS" 2>/dev/null || echo "(none)"
    echo "--- nfs-server ---"; systemctl is-active nfs-server.service 2>&1 || true
    echo "--- exported now ---"; sudo exportfs -v 2>&1 | grep -F -A1 "$DST" || echo "(not exported)"
    echo "--- nfs / mountd versions on offer ---"; rpcinfo -p localhost 2>/dev/null | awk '$5 == "nfs" || $5 == "mountd"' || echo "(no rpcinfo)"
    echo "--- initramfs /etc/nfsroot ---"; cat "$SRC/etc/nfsroot" 2>/dev/null || echo "(none: device will boot from RAM)"
    echo "--- build/keep copy (survives ./cascadia rootfs) ---"; cat "$KEEP/etc/nfsroot" 2>/dev/null || echo "(none)"
    ;;
*)
    sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
