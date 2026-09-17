#!/usr/bin/env bash
#
# Put the iPad's root filesystem on this Mac and export it over the ECM link.
#
#   ./tools/mac-nfs-export.sh on      # copy the rootfs, export it, point /init at it
#   ./tools/mac-nfs-export.sh off     # stop exporting, go back to the RAM rootfs
#   ./tools/mac-nfs-export.sh status
#
# Why: the initramfs lives in RAM, so it is also the disk.  `apk add` evaporates
# on reboot and 512 MB is the ceiling for the system and its data together.  An
# NFS root fixes both, over a link that already exists.
#
# v3, because macOS's nfsd serves v2/v3 -- neither exports(5) nor nfsd(8) on
# macOS mentions v4.  v3 needs a userspace RPC handshake for the root file
# handle, which is why mount.nfs has to be in the initramfs:
#   bash scripts/add-apk-packages.sh nfs-utils
set -euo pipefail

IBSS="${IBSS:-$HOME/iBSSloader}"
SRC="$IBSS/build/initramfs-root"
DST="${DST:-$HOME/cascadia-root}"          # deliberately not under Desktop/Documents/
                                           # Downloads: those are TCC-protected and
                                           # nfsd cannot read them
LAN_NET="${LAN_NET:-10.55.0.0}"
LAN_MASK="${LAN_MASK:-255.255.255.0}"
IP_HOST="${IP_HOST:-10.55.0.1}"
MARK="# cascadia"

[ "$(uname -s)" = "Darwin" ] || { echo "this one runs on the Mac, not the device" >&2; exit 1; }
fail() { echo "error: $*" >&2; exit 1; }

case "${1:-}" in
on)
    [ -d "$SRC" ] || fail "no rootfs at $SRC (set IBSS=)"
    [ -x "$SRC/sbin/p105-stage2" ] || fail "$SRC has no /sbin/p105-stage2 -- stage 1 would refuse it"
    [ -x "$SRC/sbin/mount.nfs" ] || echo "warning: no mount.nfs in the initramfs yet -- run: bash $IBSS/scripts/add-apk-packages.sh nfs-utils" >&2

    echo "==> copying $SRC -> $DST"
    sudo mkdir -p "$DST"
    # Device nodes are excluded on purpose: stage 1 mount --moves devtmpfs onto
    # the new root, so an empty /dev is all that is wanted, and macOS has no
    # business trying to reproduce Linux major/minor numbers.  Same for the
    # kernel's own filesystems.
    sudo rsync -a --delete \
        --exclude 'dev/*' --exclude 'proc/*' --exclude 'sys/*' --exclude 'newroot' \
        "$SRC/" "$DST/"
    # The source tree came out of a Docker bind mount and is owned by this user.
    # The device runs as uid 0, and with -maproot=root its writes land as uid 0,
    # so make the existing files agree rather than leaving a tree half-owned.
    sudo chown -R root:wheel "$DST"
    sudo mkdir -p "$DST/dev" "$DST/proc" "$DST/sys"

    echo "==> exporting to $LAN_NET/$LAN_MASK"
    # Rewrite only our own line, so anything else in /etc/exports survives.
    sudo touch /etc/exports
    sudo cp /etc/exports "/etc/exports.cascadia-backup.$(date +%s)"
    TMP=$(mktemp)
    grep -v "$MARK" /etc/exports > "$TMP" || true
    echo "$DST -maproot=root:wheel -network $LAN_NET -mask $LAN_MASK $MARK" >> "$TMP"
    sudo cp "$TMP" /etc/exports; rm -f "$TMP"

    sudo nfsd enable  >/dev/null 2>&1 || true
    sudo nfsd start   >/dev/null 2>&1 || true
    sudo nfsd update  >/dev/null 2>&1 || true
    sleep 1
    sudo nfsd checkexports || fail "nfsd rejected the export -- see the message above"
    showmount -e localhost || true

    # Stage 1 reads this at boot; absent, it stays in RAM.  It goes in the
    # initramfs, so it takes effect on the NEXT kernel build, not this instant.
    echo "$IP_HOST:$DST" > "$SRC/etc/nfsroot"
    echo
    echo "==> /etc/nfsroot in the initramfs = $IP_HOST:$DST"
    echo "    Rebuild and flash for it to take effect:"
    echo "      ./tools/rebuild-rearm.sh && ./tools/flash-rearm.sh"
    ;;
off)
    TMP=$(mktemp)
    grep -v "$MARK" /etc/exports > "$TMP" 2>/dev/null || true
    sudo cp "$TMP" /etc/exports; rm -f "$TMP"
    sudo nfsd update >/dev/null 2>&1 || true
    rm -f "$SRC/etc/nfsroot"
    echo "==> export removed and /etc/nfsroot cleared."
    echo "    $DST is left on disk; delete it yourself if you want it gone."
    echo "    Rebuild and flash to go back to the RAM rootfs."
    ;;
status)
    echo "--- /etc/exports (ours) ---"; grep "$MARK" /etc/exports 2>/dev/null || echo "(none)"
    echo "--- nfsd ---"; sudo nfsd status 2>&1 | head -3 || true
    echo "--- exported now ---"; showmount -e localhost 2>&1 | head -5 || true
    echo "--- initramfs /etc/nfsroot ---"; cat "$SRC/etc/nfsroot" 2>/dev/null || echo "(none: device will boot from RAM)"
    ;;
*)
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
