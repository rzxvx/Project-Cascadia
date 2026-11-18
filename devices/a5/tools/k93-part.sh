#!/usr/bin/env bash
#
# Linux partitions on the iPad 2's NAND, the way CoolBooter makes room for a
# second iOS: Data's HFS+ is shrunk by iOS itself while mounted (the kernel's
# HFSIOC_RESIZE_VOLUME, tools/ios/hfs-resize), then the LwVM partition table
# gets a Linux root and a swap partition in the chunks Data gave up
# (tools/k93-lwvm.py plans it and runs iOS's own check on it).  A running iOS
# will not let anyone write its table (EBUSY on rdisk0 and rdisk0s1), so Linux
# writes it -- both copies, LPN 0 and LPN 4095 -- through iOS's FTL
# (tools/nand/k93yaftl.c lwvm-apply), from a RAM root, and iOS restarts.
#
#   tools/k93-part.sh status              the table, Data's use, the room left
#   tools/k93-part.sh create ROOT SWAP    e.g. 8G 1G (or MB: 8192 1024);
#                                         rounded up to whole 32 MB chunks;
#                                         Data keeps KEEP_FREE MB free (2048)
#   tools/k93-part.sh apply NEW OLD       write a planned table (what create
#                                         does at its end), then restart iOS
#   tools/k93-part.sh remove              the Linux partitions dropped, their
#                                         space back to Data
#
# Then, in Linux: cascadia-install-partition --yes (the root moved into the
# Linux partition, the Swap partition made swap).
# The iPad: jailbroken iOS 6.1.3 on the cable, OpenSSH (as for k93-pcless.sh).
# Add --yes to skip the question.  The tables before and after are kept in
# build/k93/part/.  A restore (Legacy iOS Kit) undoes all of it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/build/k93/part"
HELPER="$ROOT/build/k93/ios-tools/hfs-resize"
IOS_HELPER=/usr/local/cascadia/hfs-resize
KEEP_FREE=${KEEP_FREE:-2048}
YES=0

say()  { printf '==> %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }
lwvm() { python3 "$ROOT/tools/k93-lwvm.py" "$@"; }

mb() {          # 8G, 512M, 1024 (MB) -> MB
    case "$1" in
        *[gG]) echo $(( ${1%[gG]} * 1024 )) ;;
        *[mM]) echo $(( ${1%[mM]} )) ;;
        *[0-9]) echo $(( $1 )) ;;
        *) fail "size '$1': use e.g. 8G, 512M or 1024 (MB)" ;;
    esac
}

connect() {
    . "$ROOT/tools/ios-ssh.sh"
    BOARD_MODEL=iPad2,1
    ios_setup k93part
    ios_check_model
}

# the first 8 KB of iOS's disk0 (= YAFTL's LPN 0: the LwVM table)
get_table() {
    ios 'dd if=/dev/rdisk0 bs=8192 count=1 2>/dev/null' > "$1"
    [ "$(stat -c%s "$1")" = 8192 ] || fail "could not read the partition table (dd of /dev/rdisk0)"
}

# Data's HFS+ size and use, in bytes, from df -k (iOS has no awk: the host parses)
data_df() {
    ios 'df -k /private/var' | awk 'NR == 2 { printf "%d %d\n", $2 * 1024, $3 * 1024 }'
}

push_helper() {
    [ -s "$HELPER" ] || bash "$ROOT/tools/ios/build.sh"
    ios "mkdir -p ${IOS_HELPER%/*} && cat > $IOS_HELPER.new && chmod 755 $IOS_HELPER.new && ldid -S $IOS_HELPER.new && mv $IOS_HELPER.new $IOS_HELPER" < "$HELPER"
    ios "$IOS_HELPER probe /private/var" | grep -q 'ok, HFS answers' \
        || fail "hfs-resize's probe did not reach HFS on this iOS (see: $IOS_HELPER probe /private/var)"
}

ask() {
    [ "$YES" = 1 ] && return 0
    [ -t 0 ] || fail "not on a terminal: add --yes to go ahead"
    local r
    read -r -p "    $1 [y/N] " r
    [ "$r" = y ] || [ "$r" = Y ] || fail "stopped, nothing changed"
}

# restart iOS and wait for its ssh again (30-140 s on the iPad 2)
restart_ios() {
    say "restarting iOS"
    ios 'sync; sync; (sleep 1; reboot) > /dev/null 2>&1 &' || true
    ssh -o ControlPath="$CM" -O exit x 2>/dev/null || true
    rm -f "$CM"
    sleep 20
    local i=0
    until ios_connect >/dev/null 2>&1; do
        i=$((i + 1))
        [ $i -lt 60 ] || fail "iOS did not come back on ssh in 5 minutes"
        sleep 5
    done
    note "iOS is back"
}

# Linux on the iPad, over the USB gadget's IPv6 link-local address (no host IP needed)
linux_if() { ip -br link | awk 'tolower($3) == "02:93:00:00:00:01" { print $1 }'; }
lx() {
    local IF; IF=$(linux_if)
    [ -n "$IF" ] || return 255
    ssh -o BatchMode=yes -o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new \
        -o UserKnownHostsFile="$ROOT/build/known_hosts" -o LogLevel=ERROR "root@fe80::93:ff:fe00:2%$IF" "$@"
}

# NEW into LPN 0 and LPN 4095 from a RAM-root Linux (its NAND buses free), then iOS again
apply_table() {
    local new=$1 old=$2
    lwvm check "$new" || fail "iOS would refuse $new: not writing it"
    [ -s "$ROOT/build/k93/tools/k93yaftl" ] || fail "no build/k93/tools/k93yaftl: ./cascadia k93 build (or tools/nand/k93yaftl.c's build line)"
    local img=0
    if ios '[ -f /cascadia/root.sqfs ]'; then
        note "the root image is set aside for this boot (a RAM root leaves the NAND to the table write)"
        ios 'mv /cascadia/root.sqfs /cascadia/root.sqfs.part && sync'
        img=1
    fi
    say "starting Linux (iOS's \`linux\`)"
    # kloader takes iOS away mid-command: the ssh never returns cleanly
    ios 'sync; sync; /usr/bin/linux' >/dev/null 2>&1 &
    local lp=$!
    sleep 25
    kill $lp 2>/dev/null || true
    ssh -o ControlPath="$CM" -O exit x 2>/dev/null || true
    rm -f "$CM"
    local i=0
    until lx true 2>/dev/null; do
        i=$((i + 1)); [ $i -lt 60 ] || fail "no Linux ssh after 5 minutes (the table was NOT written)"
        sleep 5
    done
    note "Linux is up"
    lx 'cat > /tmp/k93yaftl && chmod 755 /tmp/k93yaftl' < "$ROOT/build/k93/tools/k93yaftl"
    lx 'cat > /tmp/lwvm-new.bin' < "$new"
    lx 'cat > /tmp/lwvm-old.bin' < "$old"
    say "writing the table (LPN 0 and its backup LPN 4095) through iOS's FTL"
    lx 'pidof k93yaftl k93lv k93iosnand >/dev/null && { echo "a NAND server is running: not a RAM root"; exit 1; }
        k93nand -q id > /tmp/id.log 2>&1 || { tail -n 2 /tmp/id.log; exit 1; }
        /tmp/k93yaftl lwvm-apply /tmp/lwvm-new.bin /tmp/lwvm-old.bin' | tee "$OUT/apply-$(date +%Y%m%d-%H%M%S).log" \
        | sed 's/^/    /'
    [ "${PIPESTATUS[0]}" = 0 ] || fail "the table write failed: Linux is still up, nothing restarted (log in $OUT)"
    say "restarting into iOS (its first boot rebuilds its FTL map: the full restore)"
    lx 'sync; setsid sh -c "sleep 1; reboot" > /dev/null 2>&1 < /dev/null &' || true
    sleep 20
    i=0
    until ios_connect >/dev/null 2>&1; do
        i=$((i + 1)); [ $i -lt 120 ] || fail "iOS did not come back in 10 minutes"
        sleep 5
    done
    note "iOS is back"
    [ $img = 1 ] && ios 'mv /cascadia/root.sqfs.part /cascadia/root.sqfs && sync'
    get_table "$WORK/after.bin"
    ios 'dd if=/dev/rdisk0 bs=8192 skip=4095 count=1 2>/dev/null' > "$WORK/after-bak.bin"
    cmp -s "$WORK/after.bin" "$new" || fail "iOS's LPN 0 is not the new table"
    cmp -s "$WORK/after-bak.bin" "$new" || fail "iOS's LPN 4095 (the backup) is not the new table"
    lwvm show "$WORK/after.bin"
    ios 'ls -l /dev/rdisk0s1*; df -k /private/var' | sed 's/^/    /'
}

status() {
    connect
    get_table "$WORK/table.bin"
    lwvm show "$WORK/table.bin"
    ios 'dd if=/dev/rdisk0 bs=8192 skip=4095 count=1 2>/dev/null' > "$WORK/backup.bin"
    if cmp -s "$WORK/table.bin" "$WORK/backup.bin"; then
        note "the backup table (LPN 4095) is the same"
    else
        note "the backup table (LPN 4095) DIFFERS: $(lwvm check "$WORK/backup.bin" 2>&1 | sed 's/^[^:]*: //')"
    fi
    read -r hfs used < <(data_df)
    note "Data's HFS+: $((hfs >> 20)) MB, $((used >> 20)) MB used"
    lwvm room "$WORK/table.bin" --data-hfs "$hfs" --data-used "$used" --keep-free "$KEEP_FREE" > "$WORK/room"
    local room part
    room=$(sed -n 's/^room_mb=//p' "$WORK/room")
    part=$(sed -n 's/^data_partition_bytes=//p' "$WORK/room")
    if [ -n "$part" ] && [ "$part" != "$hfs" ]; then
        note "Data's HFS+ ($hfs bytes) is not its partition's size ($part): a create stopped half-way?"
        note "grow it back with: $IOS_HELPER resize /private/var $part   (on iOS)"
    fi
    note "room for Linux + swap: ${room:-0} MB (Data keeps $KEEP_FREE MB free; KEEP_FREE=MB changes that)"
}

create() {
    local root swap
    root=$(mb "$1"); swap=$(mb "$2")
    connect
    mkdir -p "$OUT"
    local stamp; stamp=$(date +%Y%m%d-%H%M%S)
    get_table "$OUT/table-$stamp-before.bin"
    lwvm show "$OUT/table-$stamp-before.bin"
    read -r hfs used < <(data_df)
    say "plan: Linux $root MB, swap $swap MB"
    lwvm plan "$OUT/table-$stamp-before.bin" --data-hfs "$hfs" --data-used "$used" \
        --root "$root" --swap "$swap" --keep-free "$KEEP_FREE" --out "$OUT/table-$stamp-new.bin" | tee "$WORK/plan"
    local newhfs
    newhfs=$(sed -n 's/^data_hfs_bytes=//p' "$WORK/plan")
    [ -n "$newhfs" ] || fail "no plan"
    ask "Shrink Data to $((newhfs >> 20)) MB and write this table?"

    say "hfs-resize on iOS"
    push_helper
    get_table "$WORK/again.bin"
    cmp -s "$WORK/again.bin" "$OUT/table-$stamp-before.bin" || fail "the table changed under us"
    say "shrinking Data's HFS+ to $newhfs bytes (iOS moves what is in the way; can take minutes)"
    ios "$IOS_HELPER resize /private/var $newhfs"
    read -r hfs2 used2 < <(data_df)
    [ "$hfs2" = "$newhfs" ] || fail "Data is $hfs2 bytes after the resize, not $newhfs: table NOT written"
    note "Data: $((hfs2 >> 20)) MB, $((used2 >> 20)) MB used"

    apply_table "$OUT/table-$stamp-new.bin" "$OUT/table-$stamp-before.bin"
    say "done: Linux and Swap are LwVM partitions 3 and 4 (/dev/disk0s1s3, /dev/disk0s1s4 on iOS)"
    note "next, in Linux: cascadia-install-partition --yes, then reboot"
}

remove() {
    connect
    mkdir -p "$OUT"
    local stamp; stamp=$(date +%Y%m%d-%H%M%S)
    get_table "$OUT/table-$stamp-before.bin"
    lwvm show "$OUT/table-$stamp-before.bin"
    lwvm remove "$OUT/table-$stamp-before.bin" --out "$OUT/table-$stamp-removed.bin" | tee "$WORK/plan"
    local part
    part=$(sed -n 's/^data_partition_bytes=//p' "$WORK/plan")
    ask "Drop the Linux partitions (their data is lost) and give the space back to Data?"
    apply_table "$OUT/table-$stamp-removed.bin" "$OUT/table-$stamp-before.bin"
    push_helper
    say "growing Data's HFS+ to $part bytes"
    ios "$IOS_HELPER resize /private/var $part"
    ios 'df -k /private/var' | sed 's/^/    /'
}

args=()
for a in "$@"; do
    case "$a" in
        --yes|-y) YES=1 ;;
        *) args+=("$a") ;;
    esac
done
set -- "${args[@]+"${args[@]}"}"
case "${1:-}" in
    status) status ;;
    create) [ $# -eq 3 ] || fail "usage: tools/k93-part.sh create ROOT SWAP   (e.g. 8G 1G)"; create "$2" "$3" ;;
    remove) remove ;;
    apply)  [ $# -eq 3 ] || fail "usage: tools/k93-part.sh apply NEW OLD"
            connect; mkdir -p "$OUT"; apply_table "$2" "$3" ;;
    *) sed -n '3,27p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
