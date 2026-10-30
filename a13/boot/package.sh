#!/bin/sh
# package.sh - glue m1n1 + bootargs + DTB + initramfs + kernel into the rkrn
# IMG4 that iBSS boots with `bootx`. m1n1 takes the kernelcache's place and
# finds the appended pieces by their magic.
#
# Payload: ./payload/ (release archive) or ../build/payload/ (built here):
#   m1n1-idevice.macho, t8030-n104.dtb, initramfs.cpio.gz, Image.gz
# Yours:   inputs/IM4M     the IM4M of YOUR device (see inputs/README.md)
#          ~/.ssh/*.pub    become root's authorized_keys on the phone
#                          (override: SSH_KEYS="a.pub b.pub")
# Output:  out/m1n1-linux.rkrn.img4
set -eu
cd "$(dirname "$0")"

if [ -d payload ]; then P=payload; else P=../build/payload; fi
for f in m1n1-idevice.macho t8030-n104.dtb initramfs.cpio.gz Image.gz; do
    [ -f "$P/$f" ] || { echo "missing $P/$f" >&2; exit 1; }
done
[ -f inputs/IM4M ] || { echo "missing inputs/IM4M (see inputs/README.md)" >&2; exit 1; }

case "$(ipsw version)" in
    *3.1.714*) ;;
    *)
        echo 'This script requires ipsw 3.1.714; inspect compression metadata for other versions.' >&2
        exit 1
        ;;
esac

mkdir -p out
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# initramfs = the image + a second cpio with this user's SSH keys (and, when
# the initramfs was built here, the persistent dropbear host key)
SSH_KEYS=${SSH_KEYS:-$(ls ~/.ssh/*.pub 2>/dev/null || true)}
[ -n "$SSH_KEYS" ] && cat $SSH_KEYS > "$tmp/authorized_keys" || : > "$tmp/authorized_keys"
set -- "root/.ssh/authorized_keys=$tmp/authorized_keys:600"
HOSTKEY=../build/ssh/dropbear_ed25519_host_key
[ -f "$HOSTKEY" ] && set -- "$@" "etc/dropbear/dropbear_ed25519_host_key=$HOSTKEY:600"
python3 overlay_cpio.py "$tmp/overlay.cpio" "$@"
{ gzip -dc "$P/initramfs.cpio.gz"; cat "$tmp/overlay.cpio"; } | gzip -9 > "$tmp/initramfs.cpio.gz"
echo "ssh keys: $(grep -c . "$tmp/authorized_keys") (from: ${SSH_KEYS:-none})"

cat "$P/m1n1-idevice.macho" bootargs.txt "$P/t8030-n104.dtb" \
    "$tmp/initramfs.cpio.gz" "$P/Image.gz" > out/m1n1-linux.macho
printf '\000\000\000\000' >> out/m1n1-linux.macho

ipsw img4 create --input out/m1n1-linux.macho --type rkrn --compress lzss \
    --im4m inputs/IM4M --output "$tmp/kernel.img4"
# ipsw 3.1.714 appends a bad ASN.1 compression record; drop it
python3 repair_lzss_img4.py "$tmp/kernel.img4" out/m1n1-linux.rkrn.img4
echo "wrote out/m1n1-linux.rkrn.img4"
