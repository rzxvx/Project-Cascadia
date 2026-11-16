#!/usr/bin/env bash
# ./cascadia a5 nand-volume -- the Linux volume on the iPad's NAND (spec 6.4, 6.5).
#
#   nand-volume status                 the LwVM layout and Data's HFS (iOS)
#   nand-volume create [SIZE]          make the Linux volume (default 8G)
#   nand-volume remove [--saved FILE]  give the space back to iOS's Data
#   nand-volume backup                 copy the Linux root to this computer (Linux)
#   nand-volume fill [TAR]             mkfs.ext4 + a root from TAR (Linux)
#   nand-volume build                  the helper, the modules, the e2fsprogs bundle
#
# create and remove are run again after each reboot they ask for; every run looks
# at the iPad, works out the next step and says what it will do before doing it.
# iOS steps go over USB (usbmuxd); Linux steps over ssh to 10.55.0.2 on the
# NFS image that has the PPN driver (h2fmi off), with nothing of the NAND loaded.
#
# State lives in build/nand-volume/: stock.bin (the two-volume header read
# before the first creation, kept for remove), from.bin / plan.bin (the header
# a header write starts from and the one it writes), state (step markers).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
T="$ROOT/tools/nand-volume"
S="$ROOT/build/nand-volume"
MAIN_KERNEL="${MAIN_KERNEL:-$HOME/Desktop/cascadia-main/devices/a5/build/linux}"
PY="${PYTHON_UNICORN:-python3}"
LINUX_HOST="${LINUX_HOST:-root@10.55.0.2}"
BOUND=15854493696
HFS_MARGIN=262144        # blocks free after a shrink (1 GiB), spec 6.4 step 1
mkdir -p "$S"

say()  { printf '==> %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }
ask()  {
    [ "${YES:-0}" = 1 ] && return 0
    local a; read -r -p "    $1 [y/N] " a || return 1
    case "$a" in y|Y|yes|да|Да|д) return 0 ;; *) return 1 ;; esac
}
lwvm() { "$PY" "$T/lwvm.py" "$@"; }
st_get() { sed -n "s/^$1=//p" "$S/state" 2>/dev/null | tail -1; }
st_set() { grep -v "^$1=" "$S/state" 2>/dev/null > "$S/state.new"; echo "$1=$2" >> "$S/state.new"; mv "$S/state.new" "$S/state"; }

lx() { ssh -o ConnectTimeout=5 -o BatchMode=yes "$LINUX_HOST" "$@"; }
in_linux() { lx true </dev/null >/dev/null 2>&1; }

ios_up() {
    . "$ROOT/tools/ios-ssh.sh"
    ios_setup nandvol
    ios_check_model
    local b; b=$(ios "sysctl -n kern.osversion" </dev/null) || fail "no iOS build"
    [ "$b" = 12H321 ] || fail "iOS $b; this is for 12H321 (8.4.1)"
}

# Both header copies from iOS's whole-media device into $1; equal or stop.
ios_read_header() {
    ios "dd if=/dev/rdisk0 bs=4096 count=1 2>/dev/null" </dev/null > "$1" || fail "reading LBA 0"
    ios "dd if=/dev/rdisk0 bs=4096 skip=4095 count=1 2>/dev/null" </dev/null > "$1.backup" || fail "reading LBA 4095"
    [ "$(wc -c < "$1" | tr -d " ")" = 4096 ] || fail "LBA 0: $(wc -c < "$1" | tr -d " ") bytes"
    cmp -s "$1" "$1.backup" || fail "the two LwVM header copies differ: a header write was interrupted.
    Boot the Linux image and run this again: it finishes or reports what is on the media."
    rm -f "$1.backup"
}

ios_helper() {
    [ -x "$S/cascadia-hfs" ] || fail "no helper: run 'nand-volume build' first"
    ios "cat > /tmp/cascadia-hfs && chmod 755 /tmp/cascadia-hfs" < "$S/cascadia-hfs" || fail "helper upload"
}
hfs_field() { sed -n "s/^$1=//p" <<< "$HFS" | tail -1; }   # the last report wins
hfs_info() { HFS=$(ios "/tmp/cascadia-hfs info" </dev/null) || fail "cascadia-hfs info: $HFS"; }
ios_fsck() {   # partition -> 0 when clean; output to this computer, never to Data
    local r
    ios "fsck_hfs -n -l /dev/rdisk0s1s$1" </dev/null > "$S/fsck-s$1.out" 2> "$S/fsck-s$1.err"
    r=$?
    cat "$S/fsck-s$1.err" "$S/fsck-s$1.out" > "$S/fsck-s$1.txt"
    return $r
}

# Spec 6.5 step 3: before the grow, Data's fsck may exit 8 with exactly the findings of
# a smaller HFS in a larger container.  Reads Data's HFS header and allocation bitmap.
undo_fsck_ok() {   # container blocks
    ios "dd if=/dev/rdisk0s1s2 bs=4096 count=1 2>/dev/null" </dev/null > "$S/data-block0.bin" || return 1
    : > "$S/data-bitmap.bin"
    local start count
    while read -r start count; do
        ios "dd if=/dev/rdisk0s1s2 bs=4096 skip=$start count=$count 2>/dev/null" </dev/null >> "$S/data-bitmap.bin" || return 1
    done < <(python3 "$T/hfs_undo_check.py" extents "$S/data-block0.bin")
    python3 "$T/hfs_undo_check.py" check "$S/fsck-s2.txt" "$S/data-block0.bin" "$S/data-bitmap.bin" "$1" | sed 's/^/    /'
    return "${PIPESTATUS[0]}"
}
ios_boottime() { ios "sysctl -n kern.boottime" </dev/null | sed -n 's/^{ sec = \([0-9]*\).*/\1/p'; }

data_container_blocks() {   # header -> Data's partition size in 4096-byte blocks
    "$PY" - "$1" <<'PY'
import struct,sys
b=open(sys.argv[1],'rb').read(); e=b[0x280:0x300]
s,en=struct.unpack_from('<QQ',e,32); print((en-s)//4096)
PY
}
nparts() { "$PY" -c "import struct,sys;print(struct.unpack_from('<I',open(sys.argv[1],'rb').read(),0x28)[0])" "$1"; }

hfs_checks() {   # target blocks -> stop unless Data's HFS looks as spec 6.4 says
    [ "$(hfs_field signature)" = HX ] || fail "Data is not HFSX: $(hfs_field signature)"
    [ "$(hfs_field blocksize)" = 4096 ] || fail "Data's block size is $(hfs_field blocksize)"
    [ $(( $(hfs_field attributes) & 0x2000 )) != 0 ] || fail "Data is not journaled"
}

# ------------------------------------------------------------------ status --
cmd_status() {
    if in_linux; then note "the iPad is in Linux: status reads from iOS"; exit 0; fi
    ios_up
    ios_read_header "$S/now.bin"
    lwvm show "$S/now.bin"
    lwvm validate "$S/now.bin" | sed 's/^/    /'
    ios_helper; hfs_info
    note "Data HFS: $(hfs_field total) blocks of $(hfs_field blocksize), $(hfs_field free) free, alternate header $(hfs_field alt_total)"
    note "Data container: $(data_container_blocks "$S/now.bin") blocks"
}

# ------------------------------------------------------------- header write --
linux_write_header() {   # from plan -> writes plan, or stops
    say "writing the LwVM header from Linux"
    [ -f "$S/from.bin" ] && [ -f "$S/plan.bin" ] || fail "no planned header: run this from iOS first"
    lwvm show "$S/plan.bin" | sed 's/^/    /'
    lx "lsmod | grep -qE '^(apple_ftl|apple_ppn|ppn_)'" </dev/null && \
        fail "NAND modules are loaded on the iPad: boot the plain Linux image (nothing of the NAND loaded)"
    local m
    for m in apple_ppn.ko ppn_lwvmw.ko; do
        [ -f "$S/mods/$m" ] || fail "no $m: run 'nand-volume build' first"
        lx "cat > /tmp/$m" < "$S/mods/$m" || fail "upload $m"
    done
    lx "cat > /tmp/lwvm-from.bin" < "$S/from.bin" && lx "cat > /tmp/lwvm-plan.bin" < "$S/plan.bin" || fail upload
    lx 'dmesg -c >/dev/null; insmod /tmp/apple_ppn.ko allow_write=1 && insmod /tmp/ppn_lwvmw.ko from=/tmp/lwvm-from.bin to=/tmp/lwvm-plan.bin dry_run=1; r=$(cat /sys/module/ppn_lwvmw/parameters/result); rmmod ppn_lwvmw; dmesg | grep lwvmw | sed "s/^\[[ 0-9.]*\] //"; exit $r' </dev/null \
        | sed 's/^/    /'
    [ "${PIPESTATUS[0]}" = 0 ] || { lx "rmmod apple_ppn" </dev/null; fail "the checks before writing failed (above): nothing was written"; }
    if ! ask "write this header (backup, then primary) and commit?"; then
        lx "rmmod apple_ppn" </dev/null; fail "stopped: nothing written"
    fi
    lx 'dmesg -c >/dev/null; insmod /tmp/ppn_lwvmw.ko from=/tmp/lwvm-from.bin to=/tmp/lwvm-plan.bin; r=$(cat /sys/module/ppn_lwvmw/parameters/result); rmmod ppn_lwvmw; rmmod apple_ppn; dmesg | grep lwvmw | sed "s/^\[[ 0-9.]*\] //"; exit $r' </dev/null \
        | tee "$S/write-$(date +%Y%m%d-%H%M%S).log" | sed 's/^/    /'
    [ "${PIPESTATUS[0]}" = 0 ] || fail "the header write failed (above).  Do NOT boot iOS; run this again from Linux."
    st_set header_written "$(shasum "$S/plan.bin" | cut -c1-40)"
    say "header written and committed"
}

# ------------------------------------------------------------------ create --
cmd_create() {
    local size="${1:-8G}"
    if in_linux; then
        [ "$(st_get flow)" = create ] || fail "no creation in progress: start it from iOS"
        [ "$(st_get shrunk_restarted)" = 1 ] || fail "Data's shrink is not confirmed after a clean restart yet: boot iOS and run this again"
        linux_write_header
        note "next: boot iOS (unlock it, cable in) and run 'nand-volume create' again to check"
        return
    fi
    ios_up
    ios_read_header "$S/now.bin"
    ios_helper; hfs_info
    if [ "$(nparts "$S/now.bin")" = 3 ]; then
        if [ -f "$S/plan.bin" ] && cmp -s "$S/now.bin" "$S/plan.bin"; then
            say "iOS booted on the new header: checking"
            ios_fsck 1 && ios_fsck 2 || fail "fsck_hfs not clean: see $S/fsck-s1.txt, fsck-s2.txt"
            [ "$(hfs_field total)" = "$(data_container_blocks "$S/plan.bin")" ] || \
                note "Data HFS $(hfs_field total) blocks, container $(data_container_blocks "$S/plan.bin") blocks"
            lwvm show "$S/now.bin" | sed 's/^/    /'
            st_set flow done-create
            say "the Linux volume exists.  Next: boot the Linux image and 'nand-volume fill'"
            return
        fi
        lwvm show "$S/now.bin" | sed 's/^/    /'
        fail "there are three volumes already"
    fi
    lwvm validate "$S/now.bin" >/dev/null || fail "iOS's own validator refuses the header on the media?"
    [ -f "$S/stock.bin" ] || cp "$S/now.bin" "$S/stock.bin"
    cmp -s "$S/now.bin" "$S/stock.bin" || note "the saved stock header differs from the media; planning from the media"
    cp "$S/now.bin" "$S/from.bin"
    local uuid=()
    [ -n "${LINUX_UUID:-}" ] && uuid=(--uuid "$LINUX_UUID")
    lwvm create "$S/from.bin" "$S/plan.bin" --size "$size" ${uuid[@]+"${uuid[@]}"} > "$S/plan.txt" \
        || { cat "$S/plan.txt"; fail "no valid plan for $size"; }
    sed 's/^/    /' "$S/plan.txt"
    local n; n=$(sed -n 's/^Data HFS target: \([0-9]*\) blocks.*/\1/p' "$S/plan.txt")
    st_set flow create; st_set target "$n"
    hfs_checks
    local total; total=$(hfs_field total)
    if [ "$total" = "$n" ]; then
        local shr; shr=$(st_get shrunk_at)
        if [ -n "$shr" ] && [ "$(ios_boottime)" -gt "$shr" ]; then
            ios_fsck 2 || fail "fsck_hfs of Data not clean after the restart: $S/fsck-s2.txt"
            [ "$(hfs_field alt_total)" = "$n" ] || fail "alternate HFS header says $(hfs_field alt_total)"
            st_set shrunk_restarted 1
            say "Data is $n blocks, clean after a restart.  Next: boot the Linux image, run 'nand-volume create' again"
        else
            say "Data is $n blocks; restart iOS cleanly (slider, then on) and run this again"
        fi
        return
    fi
    [ "$total" -gt "$n" ] || fail "Data's HFS is $total blocks, smaller than the plan's $n"
    local need=$(( total - n + HFS_MARGIN )) free; free=$(hfs_field free)
    [ "$free" -ge "$need" ] || fail "Data has $free blocks free; the shrink needs $need (with 1 GiB to spare)"
    say "checking Data before the shrink"
    ios_fsck 2 || fail "fsck_hfs -n -l of Data is not clean ($S/fsck-s2.txt): restart iOS cleanly and try again"
    note "Data: $total -> $n blocks ($(( (total - n) / 256 )) MiB to Linux); $free free"
    ask "shrink Data's HFS now (iOS keeps running; takes about a minute)?" || fail "stopped: nothing changed"
    local out; out=$(ios "/tmp/cascadia-hfs resize $n; sync; sleep 2; sync; /tmp/cascadia-hfs info" </dev/null)
    echo "$out" | sed 's/^/    /'
    HFS="$out"
    [ "$(hfs_field total)" = "$n" ] || fail "the shrink did not take (above); the LwVM header is untouched"
    st_set shrunk_at "$(date +%s)"
    say "Data shrunk.  Restart iOS cleanly (slider, then on), then run 'nand-volume create' again"
}

# ------------------------------------------------------------------ remove --
cmd_remove() {
    local saved=""
    [ "${1:-}" = --saved ] && saved="$2"
    if in_linux; then
        [ "$(st_get flow)" = remove ] || fail "no removal in progress: start it from iOS"
        linux_write_header
        note "next: boot iOS and run 'nand-volume remove' again to grow Data"
        return
    fi
    ios_up
    ios_read_header "$S/now.bin"
    ios_helper; hfs_info
    if [ "$(nparts "$S/now.bin")" = 3 ]; then
        cp "$S/now.bin" "$S/from.bin"
        [ -z "$saved" ] && [ -f "$S/stock.bin" ] && saved="$S/stock.bin"
        lwvm remove "$S/from.bin" "$S/plan.bin" ${saved:+--saved "$saved"} > "$S/plan.txt" \
            || { cat "$S/plan.txt"; fail "no valid plan to remove the Linux volume"; }
        sed 's/^/    /' "$S/plan.txt"
        note "everything on the Linux volume will be lost: 'nand-volume backup' first"
        ask "plan the removal (the header is written from Linux next)?" || fail stopped
        st_set flow remove
        say "next: boot the Linux image (nothing of the NAND loaded) and run 'nand-volume remove'"
        return
    fi
    [ "$(st_get flow)" = remove ] && [ -f "$S/plan.bin" ] && cmp -s "$S/now.bin" "$S/plan.bin" || \
        { lwvm show "$S/now.bin" | sed 's/^/    /'; fail "two volumes and no removal in progress"; }
    hfs_checks
    local total want; total=$(hfs_field total); want=$(data_container_blocks "$S/now.bin")
    if [ "$total" = "$want" ]; then
        ios_fsck 2 || fail "fsck_hfs of Data not clean: $S/fsck-s2.txt (restart iOS cleanly, run again)"
        st_set flow done-remove
        say "Data is back to $want blocks; the Linux volume is gone"
        return
    fi
    say "growing Data's HFS: $total -> $want blocks"
    ios_fsck 1 || fail "fsck_hfs of System not clean: $S/fsck-s1.txt"
    if ! ios_fsck 2; then
        note "Data's fsck exits non-zero; checking it is only the size mismatch (spec 6.5 step 3)"
        undo_fsck_ok "$want" || fail "Data's fsck shows more than the size mismatch: $S/fsck-s2.txt"
    fi
    ask "grow Data now?" || fail stopped
    local out; out=$(ios "/tmp/cascadia-hfs resize $want; sync; sleep 2; sync; /tmp/cascadia-hfs info" </dev/null)
    echo "$out" | sed 's/^/    /'
    HFS="$out"
    [ "$(hfs_field total)" = "$want" ] || fail "the grow did not take (above); iOS keeps working with the smaller HFS"
    say "Data grown.  Restart iOS cleanly, then run 'nand-volume remove' once more to check"
}

# ------------------------------------------------------------ backup / fill --
cmd_backup() {
    in_linux || fail "boot Linux from the NAND (the daily boot) first"
    local root; root=$(lx "awk '\$2==\"/\"{print \$1}' /proc/mounts" </dev/null)
    [ "$root" = /dev/ftl0p2 ] || fail "the root is $root, not the NAND's Linux volume"
    local out="$S/linux-root-$(date +%Y%m%d-%H%M%S).tar"
    say "copying the Linux root to $out"
    lx "mkdir -p /mnt/rootbind && mount --bind / /mnt/rootbind && tar -C /mnt/rootbind -cf - . ; r=\$?; umount /mnt/rootbind; exit \$r" \
        </dev/null > "$out" || fail "tar failed"
    tar -tf "$out" > "$out.list" || fail "the copy does not read back"
    note "$(wc -l < "$out.list") entries, $(du -h "$out" | cut -f1)"
}

cmd_fill() {
    local tar="${1:-}"
    in_linux || fail "boot the Linux image first"
    lx "lsmod | grep -qE '^(apple_ftl|apple_ppn|ppn_)'" </dev/null && fail "NAND modules are loaded: boot the plain Linux image"
    [ -n "$tar" ] && [ -f "$tar" ] || fail "usage: nand-volume fill ROOT.tar"
    local m
    for m in apple_ppn.ko apple_ftl.ko; do lx "cat > /tmp/$m" < "$S/mods/$m" || fail upload; done
    tar -C "$S/e2fs/root" -cf - . | lx "mkdir -p /tmp/e2fs && tar -C /tmp/e2fs -xf -" || fail "e2fsprogs upload"
    lx 'insmod /tmp/apple_ppn.ko allow_write=1 && insmod /tmp/apple_ftl.ko auto_mount=0 enable_write=1 write_volume=Linux && echo 1 > /sys/module/apple_ftl/parameters/mount && ls -l /dev/ftl0p2' </dev/null \
        | sed 's/^/    /' || fail "FTL mount"
    ask "format the Linux volume (ext4) and unpack $(basename "$tar") onto it?" || { lx 'echo 0 > /sys/module/apple_ftl/parameters/mount; rmmod apple_ftl apple_ppn'; fail stopped; }
    lx 'LD_LIBRARY_PATH=/tmp/e2fs/usr/lib /tmp/e2fs/sbin/mke2fs -t ext4 -F -L CascadiaLinux /dev/ftl0p2' </dev/null | sed 's/^/    /' || fail mkfs
    lx 'mkdir -p /mnt/nandlinux && mount -t ext4 -o noatime /dev/ftl0p2 /mnt/nandlinux' </dev/null || fail mount
    say "unpacking the root (a few minutes over the cable)"
    lx 'tar -C /mnt/nandlinux -xpf -' < "$tar" || fail untar
    lx 'sync; umount /mnt/nandlinux && LD_LIBRARY_PATH=/tmp/e2fs/usr/lib /tmp/e2fs/sbin/e2fsck -fn /dev/ftl0p2; r=$?; echo 0 > /sys/module/apple_ftl/parameters/mount; rmmod apple_ftl; rmmod apple_ppn; exit $r' </dev/null \
        | tail -2 | sed 's/^/    /'
    [ "${PIPESTATUS[0]}" = 0 ] || fail "e2fsck of the filled volume is not clean (above)"
    say "filled: the daily boot (the app or the cable image) can use it now"
}

# ------------------------------------------------------------------- build --
cmd_build() {
    say "iOS helper"
    sh "$T/ios/build.sh" >/dev/null 2>&1 || fail "cascadia-hfs build (needs Xcode and ldid)"
    say "modules for the main kernel ($MAIN_KERNEL)"
    local kdir K="$S/kbuild"; kdir=$(cd "$MAIN_KERNEL" && pwd -P) || fail "no kernel tree at $MAIN_KERNEL"
    rm -rf "$K"; mkdir -p "$K/patches/files/drivers/mtd/nand" "$K/tools" "$S/mods"
    cp -R "$ROOT/patches/files/drivers/mtd/nand/apple" "$K/patches/files/drivers/mtd/nand/"
    cp -R "$T" "$K/tools/"
    docker run --rm -v "$kdir":/kernel -v "$K":/b -w /b cascadia-build sh -c '
        set -e
        for d in patches/files/drivers/mtd/nand/apple tools/nand-volume; do
            make -s -C /kernel M=/b/$d ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- KBUILD_MODPOST_WARN=1 modules 2>&1 | grep -v "modpost:" || true
        done' || fail "module build"
    cp "$K/patches/files/drivers/mtd/nand/apple/apple_ppn.ko" "$K/patches/files/drivers/mtd/nand/apple/apple_ftl.ko" \
       "$K/tools/nand-volume/ppn_lwvmw.ko" "$S/mods/" || fail "modules missing"
    ls -l "$S/cascadia-hfs" "$S/mods" | sed 's/^/    /'
    [ -x "$S/e2fs/root/sbin/mke2fs" ] || note "no e2fsprogs bundle in $S/e2fs/root (Alpine armv7 e2fsprogs, unpacked)"
}

case "${1:-}" in
    status) cmd_status ;;
    create) shift; cmd_create "$@" ;;
    remove) shift; cmd_remove "$@" ;;
    backup) cmd_backup ;;
    fill)   shift; cmd_fill "$@" ;;
    build)  cmd_build ;;
    *) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
