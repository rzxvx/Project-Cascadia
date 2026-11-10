#!/usr/bin/env bash
#
# Linux on the iPad 2 without a computer: started from its own iOS.
#
#   tools/k93-pcless.sh build     the files, in build/k93/pcless
#   tools/k93-pcless.sh install   build, then put them on the iPad's iOS
#                                 (jailbroken, on the cable, OpenSSH)
#   tools/k93-pcless.sh remove    take them off iOS again
#
# Then, on the iPad's iOS as root (a terminal app, or ssh):
#
#   linux        the screen goes dark and Linux comes up from the NAND; its
#                `reboot` comes back to iOS
#
# How: `linux` runs kloader on 12H321's iBEC, patched (patch-ibec-upgrade.py)
# so that it boots by itself with its OTA path, `upgrade`, from iOS's System
# partition: /kernelcache (this build's bundle, loader and kernel and the
# machine's layer, in a Mach-O), /ramdisk and /devicetree at System's root.
# iBEC brings its own NAND, FTL and HFS+ code, so nothing of Linux's needs
# the NAND.  The plan and the reverse engineering, from the iPad mini, where
# it was first done: docs/research/p105-pcless-boot.md.  The K93's iBEC is
# the same iBoot build, its code sites at the same addresses.
#
# What goes where on iOS:
#   /kernelcache /ramdisk /devicetree   System's root (about 32 MB)
#   /usr/local/cascadia/kloader         Legacy iOS Kit's, the one tools/kdfu.sh
#                                       pins by hash
#   /usr/local/cascadia/iBEC.upgrade    Apple's code, patched: built here, never
#                                       shipped
#   /usr/bin/linux                      the command
# iBEC does not replay HFS+'s journal, so the files are synced, and synced
# again 40 s later, before they are trusted (as on the mini).  Run `install`
# again after every ./cascadia k93 build to put the new kernel there.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build/k93/pcless"
KLOADER="$ROOT/build/cache/kloader"
KLOADER_SHA256=bec212b4d99a1e3b9fa4add663fa884a2435045d1e47051415f4e26e197f8132
D=/usr/local/cascadia

say()  { printf '==> %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }

build() {
    local ibec="$ROOT/build/k93/bootchain-v2/iBEC.sigpatched"
    local dt="$ROOT/build/k93/firmware-12H321/DeviceTree.k93ap.img3.dec"
    local ipsw="${IPSW:-$ROOT/iPad2,1_8.4.1_12H321_Restore.ipsw}"
    [ -s "$ibec" ] && [ -s "$dt" ] || fail "no K93 boot chain yet: ./cascadia k93 firmware"
    [ -s "$ipsw" ] || fail "no $ipsw (or IPSW=/path/to/it): ./cascadia k93 firmware fetches it"
    [ "$(cat "$ROOT/output/board" 2>/dev/null)" = k93 ] && [ -s "$ROOT/output/staging-bundle.bin" ] \
        || fail "output/ holds no K93 build: ./cascadia k93 build"
    mkdir -p "$OUT"
    say "iBEC: boots by itself, from System (patch-ibec-upgrade.py)"
    python3 "$ROOT/scripts/patch-ibec-upgrade.py" "$ibec" "$OUT/iBEC.upgrade" | sed 's/^/  /'
    say "kernelcache, ramdisk, devicetree (make-pcless.py)"
    python3 "$ROOT/scripts/make-pcless.py" --board k93 --ipsw "$ipsw" \
        --bundle "$ROOT/output/staging-bundle.bin" --devicetree "$dt" --out "$OUT"
}

linux_cmd() {
    cat <<'EOF'
#!/bin/sh
# linux -- boot the iPad 2 into Linux (Project Cascadia).  kloader starts a
# patched iBEC, which loads /kernelcache, /ramdisk and /devicetree from this
# partition and starts the kernel: no computer needed.  The screen goes dark
# for a few seconds; `reboot` in Linux comes back to iOS.
D=/usr/local/cascadia
for f in /kernelcache /ramdisk /devicetree $D/kloader $D/iBEC.upgrade; do
    [ -s "$f" ] || { echo "linux: $f is missing -- tools/k93-pcless.sh install, from the computer" >&2; exit 1; }
done
echo "Booting Linux..."
sync; sync
exec $D/kloader $D/iBEC.upgrade
EOF
}

sha() { sha1sum "$1" | cut -d' ' -f1; }

install() {
    build
    if [ ! -s "$KLOADER" ] || [ "$(sha256sum "$KLOADER" | cut -d' ' -f1)" != "$KLOADER_SHA256" ]; then
        fail "no verified kloader in $KLOADER: run a kDFU flash once (./cascadia k93 flash --kdfu), which fetches it"
    fi
    . "$ROOT/tools/ios-ssh.sh"
    BOARD_MODEL=iPad2,1
    ios_setup k93pcless
    ios_check_model

    local need free
    need=$(( ($(stat -c%s "$OUT/kernelcache") + $(stat -c%s "$OUT/ramdisk") + $(stat -c%s "$OUT/devicetree") \
             + $(stat -c%s "$OUT/iBEC.upgrade") + $(stat -c%s "$KLOADER")) / 1024 + 1024 ))
    # what is there already will be replaced: count it as free.  (iOS here has
    # no awk, wc, cut or sha1sum: it only answers, the host does the sums.)
    free=$(ios 'df -k /; for f in /kernelcache /ramdisk /devicetree; do [ -e $f ] && ls -l $f; done; true' \
           | awk 'NR == 2 {f = $4; next} NR > 2 {f += int($5 / 1024)} END {print f + 0}')
    note "System: ${free} KB free (with the old files), ${need} KB needed"
    [ "$free" -gt "$need" ] || fail "not enough room on iOS's System partition"

    if ios 'mount | grep " on / " | grep -q read-only'; then
        note "System is read-only: remounting it read-write"
        ios 'mount -uw /'
    fi

    say "copying to the iPad's System partition"
    local f
    for f in kernelcache ramdisk devicetree; do
        ios "cat > /$f.new" < "$OUT/$f"
    done
    ios "mkdir -p $D && cat > $D/kloader.new && chmod 755 $D/kloader.new" < "$KLOADER"
    ios "cat > $D/iBEC.upgrade.new" < "$OUT/iBEC.upgrade"
    linux_cmd | ios "cat > /usr/bin/linux.new && chmod 755 /usr/bin/linux.new"

    say "checking what arrived"
    local want got
    for f in kernelcache ramdisk devicetree; do
        want="$(stat -c%s "$OUT/$f") $(sha "$OUT/$f")"
        got=$(ios "ls -l /$f.new; openssl sha1 /$f.new" \
              | awk 'NR == 1 {size = $5} /^SHA1/ {sub(/.*= /, ""); sum = $0} END {print size " " sum}')
        [ "$got" = "$want" ] || fail "/$f arrived as '$got', not '$want' -- nothing replaced"
    done
    ios "for f in /kernelcache /ramdisk /devicetree $D/kloader $D/iBEC.upgrade /usr/bin/linux; do mv \$f.new \$f; done"
    note "ok: /kernelcache /ramdisk /devicetree, $D/kloader, $D/iBEC.upgrade, /usr/bin/linux"

    say "syncing (iBEC does not replay the HFS+ journal): sync, 40 s, sync"
    ios 'sync; sleep 40; sync'
    say "done.  On the iPad's iOS, as root: linux"
}

remove() {
    . "$ROOT/tools/ios-ssh.sh"
    BOARD_MODEL=iPad2,1
    ios_setup k93pcless
    ios_check_model
    ios "rm -f /kernelcache /ramdisk /devicetree /usr/bin/linux; rm -rf $D; sync"
    say "removed"
}

case "${1:-}" in
    build) build ;;
    install) install ;;
    remove) remove ;;
    *) sed -n '3,9p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
