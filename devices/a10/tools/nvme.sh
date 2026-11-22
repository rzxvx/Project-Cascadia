#!/usr/bin/env bash
# nvme.sh - ./cascadia a10 nvme: the Linux partition (./cascadia a10 storage
# linux made it) as Linux's root. Runs on the Mac, talks to Linux on the iPad
# (ssh root@172.16.43.1).
#
#   ./cascadia a10 nvme setup    read-only: find the "Linux" GPT entry on the
#                                iPad, check it against the kernel's view, and
#                                note it in build/private/nvme-linux -- pack
#                                then opens the kernel's write gate for that
#                                range only (apple,write-ranges in the DT)
#   ./cascadia a10 nvme format   with the gate open (boot after setup + pack):
#                                ext4 labelled cascadia-root on the partition,
#                                the Alpine root (build rootfs) unpacked onto
#                                it; asks for a typed FORMAT. /init then boots
#                                from it, before NFS and RAM.
#   ./cascadia a10 nvme status   what the iPad's kernel and /init made of it
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
A10="$(cd "$HERE/.." && pwd)"
NOTE="$A10/build/private/nvme-linux"
HOST=${HOST:-root@172.16.43.1}
LINUX_TYPE=0FC63DAF-8483-4772-8E79-3D69D8477DE4

say()  { printf '==> %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }
ipad() { ssh -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR "$HOST" "$@"; }

# build/private/nvme-linux -> n_nsid n_first n_count n_part n_guid (checked
# first: bash 3.2 on macOS cannot source a process substitution)
nvme_note() {
    grep -Eqx 'nsid=[0-9]+ first=[0-9]+ count=[0-9]+ part=nvme0n1p[0-9]+ guid=[0-9A-F-]+' "$1" \
        || fail "$1 is not what nvme setup writes"
    eval "$(tr ' ' '\n' < "$1" | sed 's/^/n_/')"
}

ipad true </dev/null 2>/dev/null || fail "no Linux on the iPad at ${HOST#*@} -- boot it: ./cascadia a10 boot"

case "${1:-}" in
setup)
    say "the GPT on the iPad (read-only)"
    # the 128 entries after the primary header: LBA 2 of 4096-byte sectors
    raw=$(ipad 'dd if=/dev/nvme0n1 bs=4096 skip=2 count=4 2>/dev/null | od -An -v -tx1' </dev/null | tr -d ' \n')
    sysfs=$(ipad 'for p in /sys/class/block/nvme0n1p*; do echo "$(basename $p) $(cat $p/start) $(cat $p/size) $(grep ^PARTNAME= $p/uevent | cut -d= -f2)"; done' </dev/null)
    lbs=$(ipad 'cat /sys/block/nvme0n1/queue/logical_block_size' </dev/null)
    [ "$lbs" = 4096 ] || fail "unexpected NVMe block size $lbs"
    found=$(python3 -I - "$raw" "$LINUX_TYPE" <<'PY'
import sys, struct, uuid
raw = bytes.fromhex(sys.argv[1]); want = sys.argv[2]
hits = []
for i in range(128):
    e = raw[128 * i:128 * (i + 1)]
    if e[:16] == bytes(16):
        continue
    t = str(uuid.UUID(bytes_le=e[:16])).upper()
    g = str(uuid.UUID(bytes_le=e[16:32])).upper()
    first, last = struct.unpack('<QQ', e[32:48])
    name = e[56:128].decode('utf-16-le').split('\0')[0]
    print(f"part {i + 1} type={t} first={first} last={last} name={name}", file=sys.stderr)
    if t == want and name == 'Linux':
        hits.append(f"{i + 1} {first} {last - first + 1} {g}")
print('\n'.join(hits))
PY
)
    [ "$(printf '%s\n' "$found" | grep -c .)" = 1 ] || fail "need exactly one \"Linux\" GPT entry, found: ${found:-none}"
    read -r idx first count guid <<<"$found"
    # the kernel's view of the same partition, in 512-byte sectors
    k=$(printf '%s\n' "$sysfs" | awk -v p="nvme0n1p$idx" '$1 == p')
    [ -n "$k" ] || fail "the kernel has no nvme0n1p$idx"
    read -r _ kstart ksize kname <<<"$k"
    [ "$((kstart / 8))" = "$first" ] && [ "$((ksize / 8))" = "$count" ] && [ "$kname" = Linux ] \
        || fail "the kernel sees nvme0n1p$idx differently ($k) -- refusing"
    mkdir -p "$A10/build/private"; chmod 700 "$A10/build/private"
    printf 'nsid=1 first=%s count=%s part=nvme0n1p%s guid=%s\n' "$first" "$count" "$idx" "$guid" > "$NOTE"
    say "Linux partition: nvme0n1p$idx, LBA $first +$count ($((count * 4096 / 1000000000)) GB), $guid"
    note "noted in build/private/nvme-linux; next: ./cascadia a10 pack, boot, then ./cascadia a10 nvme format"
    ;;
format)
    [ -f "$NOTE" ] || fail "no build/private/nvme-linux -- run: ./cascadia a10 nvme setup"
    nvme_note "$NOTE"
    [ -f "$A10/build/alpine-a10.tar.gz" ] || fail "no Alpine root -- run: ./cascadia a10 build rootfs"
    gate=$(ipad 'dmesg | grep "write gate"' </dev/null || true)
    printf '%s\n' "$gate" | sed 's/^/    /'
    printf '%s\n' "$gate" | grep -q "namespace 1, LBA $n_first +$n_count writable" \
        || fail "the iPad's kernel has not opened the write gate for LBA $n_first +$n_count -- pack and boot after setup"
    dev=/dev/$n_part
    [ "$(ipad "cat /sys/class/block/$n_part/ro" </dev/null)" = 0 ] || fail "$dev is read-only on the iPad"
    ipad "command -v mkfs.ext4 >/dev/null" </dev/null || fail "no mkfs.ext4 on the iPad -- boot Alpine (build rootfs, pack, boot)"
    if ipad "grep -q '^$dev ' /proc/mounts" </dev/null; then
        fail "$dev is mounted (Linux runs from it?) -- nothing to format"
    fi
    echo "    $dev ($((n_count * 4096 / 1000000000)) GB): everything on it is erased. iPadOS's APFS is not touched."
    read -r -p "    Type FORMAT to make it Linux's root: " ans || ans=
    [ "$ans" = FORMAT ] || fail "stopped -- nothing was changed"
    say "mkfs.ext4 -L cascadia-root $dev"
    ipad "mkfs.ext4 -q -F -L cascadia-root $dev && mkdir -p /mnt/nvme && mount -t ext4 $dev /mnt/nvme" </dev/null
    say "unpacking the Alpine root onto it"
    ipad "tar xzf - -C /mnt/nvme && sync && df -h /mnt/nvme | tail -1 && umount /mnt/nvme" < "$A10/build/alpine-a10.tar.gz"
    say "done: the next boot starts from $dev (the initramfs finds LABEL=cascadia-root)"
    ;;
status)
    ipad 'dmesg | grep -E "write gate|\[nvme\]"; for d in /sys/class/block/nvme0n*; do printf "%s ro=%s\n" "${d##*/}" "$(cat $d/ro)"; done; grep nvme /proc/mounts' </dev/null
    ;;
*)
    fail "nvme what? setup, format, status"
    ;;
esac
