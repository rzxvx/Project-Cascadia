#!/usr/bin/env bash
#
# Put the iPad's root filesystem on this Linux host and export it over the ECM
# link.  The Linux twin of mac-nfs-export.sh; ./cascadia a5 nfs picks by host.
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
SRC="${SRC:-$ROOT/build/initramfs-root}"   # the tree ./cascadia a5 rootfs builds and
                                           # ./cascadia a5 build embeds
DESKTOP_TAR="$ROOT/build/desktop-root.tar"  # ./cascadia a5 desktop-root
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
    if systemctl is-active --quiet firewalld 2>/dev/null; then f=firewalld; fi
    # Not systemctl for ufw: ufw.service stays "active (exited)" with the
    # firewall disabled, and said "ufw is running" on a host where it was off.
    # The switch is ENABLED in its own configuration.
    if grep -qs '^ENABLED=yes' /etc/ufw/ufw.conf; then f=ufw; fi
    [ -n "$f" ] || return 0
    echo "note: $f is running.  The device needs TCP 111 (rpcbind), 2049 (nfsd) and"
    echo "      mountd's port from $LAN; if the mount times out at boot, allow them there."
}

case "${1:-}" in
on)
    [ -d "$SRC" ] || fail "no rootfs at $SRC -- run ./cascadia a5 rootfs first (or set SRC=)"
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
    elif [ -f "$DESKTOP_TAR" ] && [ "${DESKTOP:-1}" != 0 ]; then
        # The root with the desktop (./cascadia a5 desktop-root): built in a
        # Linux container, owners and modes already right, kept by number.
        echo "==> seeding $DST from $DESKTOP_TAR (the desktop root; DESKTOP=0 for the plain one)"
        # FORCE_SYNC over a populated root: start it empty, as rsync --delete
        # does on the other path -- unpacked over the old one, the two mix
        if [ -n "$(ls -A "$DST" 2>/dev/null)" ]; then
            case "$DST" in /|"$HOME"|"") fail "refusing to empty $DST" ;; esac
            echo "    emptying $DST first (FORCE_SYNC=1)"
            sudo find "$DST" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
        fi
        sudo mkdir -p "$DST"
        sudo tar -xpf "$DESKTOP_TAR" --numeric-owner -C "$DST"
        # a tar from before make-root marked its roots still is the desktop's
        [ -s "$DST/etc/cascadia-root" ] ||
            echo "desktop unknown $(date -u +%Y-%m-%d)" | sudo tee "$DST/etc/cascadia-root" >/dev/null
        sudo mkdir -p "$DST/dev" "$DST/proc" "$DST/sys"
    else
        echo "==> copying $SRC -> $DST"
        [ -f "$DESKTOP_TAR" ] || echo "    (no desktop yet: ./cascadia a5 desktop-root builds a root with XFCE, the GPU, SuperTux)"
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

    # Stage 1 reads this at boot; absent, it stays in RAM.  It goes in the
    # local layer (build/keep -> build/overlay), which the bundle carries
    # next to the kernel: the next ./cascadia a5 build puts it in, in seconds
    # with a prebuilt kernel.  Never into build/initramfs-root, the base a
    # kernel embeds and a release ships.
    mkdir -p "$KEEP/etc"
    echo "$IP_HOST:$DST" > "$KEEP/etc/nfsroot"
    echo
    echo "==> /etc/nfsroot in the local layer = $IP_HOST:$DST"
    echo "    Rebuild and flash for it to take effect:"
    echo "      ./cascadia a5 build && ./cascadia a5 flash"
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
    echo "--- the image's /etc/nfsroot (build/keep, the local layer) ---"; cat "$KEEP/etc/nfsroot" 2>/dev/null || echo "(none: the device boots from RAM)"
    ;;
*)
    sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
