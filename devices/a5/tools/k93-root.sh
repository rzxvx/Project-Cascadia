#!/usr/bin/env bash
#
# The iPad 2's big root, on its own NAND: a squashfs image on iOS's System
# partition, which Linux reads straight off the flash.
#
#   tools/k93-root.sh tree      build/k93/root.tar: the desktop root (XFCE with
#                               touch, Mesa on the GPU, the GPU's pack,
#                               screenpad, YouTube), built on this machine by
#                               tools/desktop/make-root.sh, without SuperTux
#   tools/k93-root.sh build     build/k93/root.sqfs from build/k93/root.tar
#   tools/k93-root.sh install   build, then put it on the iPad's iOS as
#                               /cascadia/root.sqfs (jailbroken, on the cable,
#                               OpenSSH -- as tools/k93-pcless.sh)
#   tools/k93-root.sh remove    take it off iOS again
#
# At boot (initramfs/init, "NAND root") k93lv serves iOS's System off the raw
# NAND as /dev/nbd0 and the Linux volume as /dev/nbd1, System mounts
# read-only on /mnt/ios, and the image becomes the root under an overlay
# whose changes are kept on the Linux volume (tools/nand/k93lv.c: blocks of
# the NAND that iOS keeps free and never reaches; RAM with cascadia.lv=off).
# Without the image, or with cascadia.root=ram on the kernel command line,
# the iPad boots the RAM root as before.  Linux never writes iOS's own data:
# iOS writes the image, and iOS stays as it was.
#
# The image is the tree plus /etc/cascadia/desktop-at-boot: stage 2 starts
# XFCE by itself on this root.
#
# zstd at level 19: about xz's size, and it decompresses several times faster
# on the A9 (the kernel needs CONFIG_SQUASHFS_ZSTD, config/k93ap.config).
# System has ~440 MB free on a stock 6.1.3; the install keeps 40 MB of it for
# iOS (Cydia installs there).  Linux's hfsplus does not replay HFS+'s journal,
# so the image is synced, and synced again 40 s later, before it is trusted --
# as the pcless files are for iBEC.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TAR="$ROOT/build/k93/root.tar"
IMG="$ROOT/build/k93/root.sqfs"
RESERVE_KB=$((40 * 1024))
STEPS="xfce patched autorotate mesa gpu screenpad youtube"

say()  { printf '==> %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }

tree() {
    say "the desktop root, in docker volume cascadia-k93-root ($STEPS)"
    VOL=cascadia-k93-root OUT="$TAR" ONLY="$STEPS" bash "$ROOT/tools/desktop/make-root.sh"
}

build() {
    if [ -s "$IMG" ] && { [ ! -e "$TAR" ] || [ "$IMG" -nt "$TAR" ]; }; then
        note "$IMG is newer than the tree: kept"
        return 0
    fi
    [ -s "$TAR" ] || fail "no $TAR: tools/k93-root.sh tree"
    say "squashfs (zstd 19) from $TAR"
    # sqfstar takes the tar as it is: owners, modes and devices from the tar,
    # not from this machine.  On the host's own architecture, no emulation.
    local plat
    plat=linux/$(uname -m | sed 's/x86_64/amd64/; s/aarch64/arm64/')
    docker run --rm -i --platform "$plat" -v "$ROOT/build/k93":/w alpine:3.24 sh -c \
        'apk add -q squashfs-tools >/dev/null && rm -f /w/root.sqfs.part &&
         sqfstar -quiet -comp zstd -Xcompression-level 19 -b 128K -root-mode 755 -root-uid 0 -root-gid 0 \
             -p "etc/cascadia d 755 0 0" -p "etc/cascadia/desktop-at-boot f 644 0 0 true" \
             /w/root.sqfs.part < /w/root.tar &&
         chown '"$(id -u):$(id -g)"' /w/root.sqfs.part' \
        || fail "sqfstar failed (above)"
    mv "$ROOT/build/k93/root.sqfs.part" "$IMG"
    note "$(du -m "$IMG" | cut -f1) MB, from a $(du -m "$TAR" | cut -f1) MB tree"
}

sha() { sha1sum "$1" | cut -d' ' -f1; }

install() {
    build
    . "$ROOT/tools/ios-ssh.sh"
    BOARD_MODEL=iPad2,1
    ios_setup k93root
    ios_check_model

    local size need free old
    size=$(( $(stat -c%s "$IMG") / 1024 + 1 ))
    # (iOS here has no awk, wc, cut or sha1sum: it only answers, the host
    # does the sums)
    read -r free old < <(ios 'df -k /; [ -e /cascadia/root.sqfs ] && ls -l /cascadia/root.sqfs; true' \
        | awk 'NR == 2 {f = $4} NR > 2 {o = int($5 / 1024)} END {print f + 0, o + 0}')
    note "System: ${free} KB free, ${old} KB in the image there now, ${size} KB to copy"
    if [ $((free - size)) -ge "$RESERVE_KB" ]; then
        :       # room for both: the old one stays until the new one checks out
    elif [ $((free + old - size)) -ge "$RESERVE_KB" ]; then
        note "room for one image only: the old one goes first (until the copy is done"
        note "the iPad boots the RAM root)"
        ios 'rm -f /cascadia/root.sqfs'
    else
        fail "not enough room on iOS's System partition: $((size / 1024)) MB, and $((RESERVE_KB / 1024)) MB left for iOS"
    fi

    if ios 'mount | grep " on / " | grep -q read-only'; then
        note "System is read-only: remounting it read-write"
        ios 'mount -uw /'
    fi

    say "copying to the iPad's System partition ($((size / 1024)) MB)"
    local t0=$SECONDS
    ios 'mkdir -p /cascadia && cat > /cascadia/root.sqfs.new' < "$IMG"
    note "$((SECONDS - t0)) s"

    say "checking what arrived"
    local want got
    want="$(stat -c%s "$IMG") $(sha "$IMG")"
    got=$(ios 'ls -l /cascadia/root.sqfs.new; openssl sha1 /cascadia/root.sqfs.new' \
          | awk 'NR == 1 {size = $5} /^SHA1/ {sub(/.*= /, ""); sum = $0} END {print size " " sum}')
    [ "$got" = "$want" ] || fail "/cascadia/root.sqfs.new arrived as '$got', not '$want' -- nothing replaced"
    ios 'mv /cascadia/root.sqfs.new /cascadia/root.sqfs'
    note "ok: /cascadia/root.sqfs"

    say "syncing (Linux's hfsplus does not replay the HFS+ journal): sync, 40 s, sync"
    ios 'sync; sleep 40; sync'
    say "done.  The next Linux boot (\`linux\` on iOS, or a flash) comes up on it."
}

remove() {
    . "$ROOT/tools/ios-ssh.sh"
    BOARD_MODEL=iPad2,1
    ios_setup k93root
    ios_check_model
    ios 'rm -rf /cascadia; sync'
    say "removed: Linux boots the RAM root again"
}

case "${1:-}" in
    tree) tree ;;
    build) build ;;
    install) install ;;
    remove) remove ;;
    *) sed -n '3,15p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
