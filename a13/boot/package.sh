#!/bin/sh
# package.sh - glue m1n1 + bootargs + DTB + initramfs + kernel into the rkrn
# IMG4 that iBSS boots with `bootx`. m1n1 takes the kernelcache's place and
# finds the appended pieces by their magic.
#
# Payload: ./payload/ (release archive) or ../build/payload/ (built here):
#   m1n1-idevice.macho, t8030-n104.dtb, initramfs.cpio.gz, Image.gz
# Yours:   inputs/IM4M     the IM4M of YOUR device (see inputs/README.md)
#          from a13/cascadia periphery: inputs/n104-mt*.bin (touch) and
#          inputs/brcm/ (Wi-Fi/BT) into /lib/firmware, inputs/syscfg.bin
#          (this phone's Wi-Fi/BT calibration and addresses) into the DT,
#          ../build/fbkeyboard/ (the on-screen keyboard) and
#          ../build/wifitools/ (iw, wpa_supplicant: /opt/wifi) into the initramfs
#          ~/.ssh/*.pub    become root's authorized_keys on the phone
#                          (override: SSH_KEYS="a.pub b.pub")
#          NFS root        optional, see ../host/mac-nfs-export.sh (NFSROOT=)
#          ../build/alpine-a13.tar.gz (a13/cascadia build rootfs): the root
#                          the phone runs in RAM when there is no NFS root
# Output:  out/m1n1-linux.rkrn.img4, out/alpine.rdsk.img4 (the Alpine root as
#          an iBoot ramdisk: iBoot refuses a large kernel image)
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
# NFS root ("host:/path"): NFSROOT=..., or the one ../host/mac-nfs-export.sh on
# wrote; NFSROOT=none packs a RAM-only image
[ -z "${NFSROOT:-}" ] && [ -f ../build/nfsroot ] && NFSROOT=$(cat ../build/nfsroot)
if [ -n "${NFSROOT:-}" ] && [ "$NFSROOT" != none ]; then
    echo "$NFSROOT" > "$tmp/nfsroot"
    set -- "$@" "etc/nfsroot=$tmp/nfsroot:644"
fi
echo "nfs root: ${NFSROOT:-none}"
# the peripherals (a13/cascadia periphery): touch and Wi-Fi/BT firmware where
# the drivers ask for them, the on-screen keyboard
for f in n104-mtfw.bin n104-mtcal.bin; do
    if [ -f "inputs/$f" ]; then
        set -- "$@" "lib/firmware/apple/$f=inputs/$f:644"
    else
        echo "note: no inputs/$f -- no touchscreen (run: a13/cascadia periphery)" >&2
    fi
done
if [ -d inputs/brcm ]; then
    for f in inputs/brcm/*; do set -- "$@" "lib/firmware/brcm/${f##*/}=$f:644"; done
else
    echo "note: no inputs/brcm -- no Wi-Fi (run: a13/cascadia periphery)" >&2
fi
if [ -d ../build/fbkeyboard ]; then
    for f in $(cd ../build/fbkeyboard && find . -type f | sed 's|^\./||'); do
        case "$f" in usr/bin/*|lib/ld-*) m=755 ;; *) m=644 ;; esac
        set -- "$@" "$f=../build/fbkeyboard/$f:$m"
    done
fi
if [ -d ../build/wifitools ]; then
    for f in $(cd ../build/wifitools && find . -type f | sed 's|^\./||'); do
        case "$f" in */bin/*|*/libexec/*|*/lib/ld-*|*.script) m=755 ;; *) m=644 ;; esac
        set -- "$@" "$f=../build/wifitools/$f:$m"
    done
fi
python3 overlay_cpio.py "$tmp/overlay.cpio" "$@"
{ gzip -dc "$P/initramfs.cpio.gz"; cat "$tmp/overlay.cpio"; } | gzip -9 > "$tmp/initramfs.cpio.gz"
echo "ssh keys: $(grep -c . "$tmp/authorized_keys") (from: ${SSH_KEYS:-none})"

# Wi-Fi / Bluetooth data from the phone's syscfg (a13/cascadia periphery): this
# boot chain does not put it into the ADT, so m1n1 cannot copy it. m1n1 still
# overrides these when the ADT has them.
cp "$P/t8030-n104.dtb" "$tmp/dtb"
wifi=$(fdtget "$tmp/dtb" /aliases wifi0 2>/dev/null || true)
if [ -f inputs/syscfg.bin ] && [ -n "$wifi" ]; then
    sc() { python3 -I ../tools/syscfg.py inputs/syscfg.bin get "$1" "$tmp/$1"; }
    hexbytes() { od -An -v -tx1 "$1" | tr -s ' \n' ' '; }
    bt=$(fdtget "$tmp/dtb" /aliases bluetooth0)
    sc WMac && fdtput -t bx "$tmp/dtb" "$wifi" local-mac-address $(head -c 6 "$tmp/WMac" | od -An -v -tx1)
    sc WSKU && fdtput -t s "$tmp/dtb" "$wifi" apple,antenna-sku "$(dd if="$tmp/WSKU" bs=1 skip=8 count=8 2>/dev/null | tr -d '\0')"
    sc WCAL && fdtput -t bx "$tmp/dtb" "$wifi" brcm,cal-blob $(hexbytes "$tmp/WCAL")
    # the BD address goes in reversed, as m1n1 does it
    sc BMac && fdtput -t bx "$tmp/dtb" "$bt" local-bd-address $(head -c 6 "$tmp/BMac" | od -An -v -tx1 | awk '{for (i = NF; i > 0; i--) printf "%s ", $i}')
    sc BCAL && fdtput -t bx "$tmp/dtb" "$bt" brcm,taurus-cal-blob $(hexbytes "$tmp/BCAL")
    echo "syscfg: Wi-Fi MAC, antenna $(fdtget -t s "$tmp/dtb" "$wifi" apple,antenna-sku), calibration $(wc -c < "$tmp/WCAL") bytes; BT address, calibration"
elif [ -n "$wifi" ]; then
    echo "note: no inputs/syscfg.bin -- Wi-Fi without calibration (boot, then: a13/cascadia periphery)" >&2
fi

# the internal NVMe given to Linux (a13/cascadia periphery: inputs/nvme-linux):
# the NVMe driver's write gate opens namespace 1, all of it; the small
# namespaces (firmware, syscfg, nvram, effaceable) stay closed
nvme=$(fdtget -l "$tmp/dtb" /soc 2>/dev/null | grep '^nvme@' | head -n 1 || true)
if [ -f inputs/nvme-linux ] && [ -n "$nvme" ]; then
    fdtput -t u "$tmp/dtb" "/soc/$nvme" apple,write-ranges 1 0 0 0 0
    echo "NVMe: namespace 1 is Linux's (the write gate opens there only)"
fi

# this phone's touchscreen calibration (a13/cascadia recalibrate)
ts=$(fdtget "$tmp/dtb" /aliases touchscreen0 2>/dev/null || true)
if [ -f inputs/touch-calibration ] && [ -n "$ts" ]; then
    . ./inputs/touch-calibration
    fdtput -t u "$tmp/dtb" "$ts" touchscreen-min-x "$MIN_X"
    fdtput -t u "$tmp/dtb" "$ts" touchscreen-size-x "$SIZE_X"
    fdtput -t u "$tmp/dtb" "$ts" touchscreen-min-y "$MIN_Y"
    fdtput -t u "$tmp/dtb" "$ts" touchscreen-size-y "$SIZE_Y"
    for a in x y; do
        fdtput -d "$tmp/dtb" "$ts" touchscreen-inverted-$a 2>/dev/null || true
    done
    [ "$INV_X" = 1 ] && fdtput -t s "$tmp/dtb" "$ts" touchscreen-inverted-x ""
    [ "$INV_Y" = 1 ] && fdtput -t s "$tmp/dtb" "$ts" touchscreen-inverted-y ""
    echo "touch: x $MIN_X..$((SIZE_X - 1))$([ "$INV_X" = 1 ] && echo ' inverted'), y $MIN_Y..$((SIZE_Y - 1))$([ "$INV_Y" = 1 ] && echo ' inverted') (inputs/touch-calibration)"
fi

cat "$P/m1n1-idevice.macho" bootargs.txt "$tmp/dtb" \
    "$tmp/initramfs.cpio.gz" "$P/Image.gz" > out/m1n1-linux.macho
printf '\000\000\000\000' >> out/m1n1-linux.macho

ipsw img4 create --input out/m1n1-linux.macho --type rkrn --compress lzss \
    --im4m inputs/IM4M --output "$tmp/kernel.img4"
# ipsw 3.1.714 appends a bad ASN.1 compression record; drop it
python3 repair_lzss_img4.py "$tmp/kernel.img4" out/m1n1-linux.rkrn.img4
echo "wrote out/m1n1-linux.rkrn.img4"

# Alpine in RAM: the initramfs unpacks it into a tmpfs when there is no NFS
# root (none packed, or the host not there: no cable) -- the same system,
# without the cable. It travels as a ramdisk (rdsk), a gzip'd cpio holding
# /alpine.tar.gz; m1n1 appends it to the initramfs (patch 0006).
rm -f out/alpine.rdsk.img4
if [ -f ../build/alpine-a13.tar.gz ]; then
    python3 overlay_cpio.py "$tmp/alpine.cpio" "alpine.tar.gz=../build/alpine-a13.tar.gz:644"
    gzip -1 < "$tmp/alpine.cpio" > "$tmp/alpine.cpio.gz"
    ipsw img4 create --input "$tmp/alpine.cpio.gz" --type rdsk \
        --im4m inputs/IM4M --output out/alpine.rdsk.img4 >/dev/null
    echo "wrote out/alpine.rdsk.img4 (Alpine in RAM when there is no NFS root)"
else
    echo "note: no build/alpine-a13.tar.gz -- without NFS the phone stays in the initramfs (a13/cascadia build rootfs)" >&2
fi
