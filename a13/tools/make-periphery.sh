#!/usr/bin/env bash
#
# make-periphery.sh - everything the phone's peripherals need, in one go
# (a13/cascadia periphery). Run it after `a13/cascadia inputs`; run it again
# any time -- what is already there is kept.
#
#   touch        the controller's firmware + default calibration, from the
#                IPSW's N104_Multitouch.im4p
#   fbkeyboard   the on-screen keyboard (asked, Enter = yes; FBKEYBOARD=y|n)
#   Wi-Fi / BT   the BCM4378 firmware from the IPSW's root filesystem (one
#                4.9 GB download, kept in build/cache), iw + wpa_supplicant
#                (Alpine) so the image can join a network on its own, and the
#                phone's own calibration, MAC addresses and antenna SKU from
#                its syscfg (read over ssh from the phone running Linux,
#                read-only)
#   NVMe         all of the internal storage for Linux, iOS erased (asked,
#                type ERASE; NVME=erase): a GPT, ext4, the Alpine root on it
#
# Output (boot/inputs/, not tracked): n104-mtfw.bin, n104-mtcal.bin, brcm/,
# syscfg.bin, nvme-linux (the decision), nvme-backup/; build/fbkeyboard/,
# build/wifitools/ (small trees for the initramfs).
# `a13/cascadia pack` puts whatever is there into the boot image.
set -euo pipefail

A13="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INPUTS="$A13/boot/inputs"
CACHE="$A13/build/cache"
mkdir -p "$INPUTS" "$CACHE"

say()  { printf '==> %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }
if have shasum; then sha256() { shasum -a 256 "$1" | awk '{print $1}'; }
else sha256() { sha256sum "$1" | awk '{print $1}'; }; fi

have ipsw    || fail "ipsw not found (https://github.com/blacktop/ipsw)"
have python3 || fail "python3 not found"

IPSW_URL="https://updates.cdn-apple.com/2023SpringFCS/fullrestores/032-67885/8B726A7C-722A-4D2D-8BEF-5A702FFEC8D5/iPhone12,1_16.4_20E247_Restore.ipsw"
MTFW_SHA256=de5dd11f00abae27b07f1a637686fcd03df748d1d4712a01592ac4bea4015036
OSDMG=078-23869-318.dmg                 # the root filesystem ("OS" in BuildManifest)
PHONE=${PHONE:-root@172.16.42.1}

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# ---------- touch -------------------------------------------------------------
# N104_Multitouch.im4p (tag mtfw, not encrypted): the controller's firmware and
# boot recipe per panel personality; the ADT names C1FC2,2. mtfw.py makes the
# "Z2FW" file the driver loads and keeps the plist's default calibration.

say "touch"
if [ -f "$INPUTS/n104-mtfw.bin" ] && [ "$(sha256 "$INPUTS/n104-mtfw.bin")" = "$MTFW_SHA256" ]; then
    note "ok: n104-mtfw.bin (already there)"
else
    python3 -I "$A13/tools/ipsw-partial.py" "$IPSW_URL" 'Firmware/N104_Multitouch.im4p' -o "$tmp" >/dev/null
    ipsw img4 im4p extract "$tmp/N104_Multitouch.im4p" -o "$tmp/mt.raw" >/dev/null
    python3 -I "$A13/tools/mtfw.py" "$tmp/mt.raw" C1FC2,2 \
        "$INPUTS/n104-mtfw.bin" "$INPUTS/n104-mtcal.bin" >/dev/null
    [ "$(sha256 "$INPUTS/n104-mtfw.bin")" = "$MTFW_SHA256" ] || fail "n104-mtfw.bin checksum mismatch"
    note "ok: n104-mtfw.bin, n104-mtcal.bin"
fi

# ---------- on-screen keyboard -------------------------------------------------
# fbkeyboard (Alpine) draws a keyboard at the bottom of the framebuffer and
# types into the console -- the only keyboard the phone has. It goes into the
# initramfs as a small self-contained tree (musl loader, libraries, font) and
# the boot starts it on the touchscreen.

say "on-screen keyboard"
ans=${FBKEYBOARD:-}
if [ -z "$ans" ]; then
    if [ -t 0 ]; then
        read -r -p "    Download fbkeyboard, the on-screen keyboard? [Y/n] " ans || ans=
    fi
    ans=${ans:-y}
fi
case "$ans" in
    [nN]*)
        rm -rf "$A13/build/fbkeyboard"
        note "skipped: no on-screen keyboard"
        ;;
    *)
        if [ -x "$A13/build/fbkeyboard/usr/bin/fbkeyboard" ]; then
            note "ok: fbkeyboard (already there)"
        else
            have docker || fail "docker is needed to fetch fbkeyboard"
            rm -rf "$A13/build/fbkeyboard"
            mkdir -p "$A13/build/fbkeyboard"
            docker run --rm --platform linux/arm64 -e HOST_IDS="$(id -u):$(id -g)" \
                -v "$A13/build/fbkeyboard":/out alpine:3.24 sh -ec '
                apk add -q fbkeyboard font-dejavu >/dev/null
                for f in /usr/bin/fbkeyboard /lib/ld-musl-aarch64.so.1 \
                         $(ldd /usr/bin/fbkeyboard | sed -n "s/.*=> \([^ ]*\).*/\1/p") \
                         /usr/share/fonts/dejavu/DejaVuSans.ttf; do
                    mkdir -p "/out$(dirname "$f")"
                    cp -L "$f" "/out$f"
                done
                chown -R "$HOST_IDS" /out' || fail "could not fetch fbkeyboard"
            note "ok: fbkeyboard + its libraries + DejaVu Sans -> build/fbkeyboard"
        fi
        ;;
esac

# ---------- Wi-Fi / Bluetooth firmware ------------------------------------------
# In the root filesystem: the whole DMG has to come down (stored in the IPSW
# zip, fetched as a byte range, resumable, kept in build/cache).

say "Wi-Fi / Bluetooth firmware"
if ls "$INPUTS/brcm"/brcmfmac4378* "$INPUTS/brcm"/brcmbt4378* >/dev/null 2>&1; then
    note "ok: brcm/ (already there)"
else
    dmg="$CACHE/$OSDMG"
    range=$(python3 -I - "$A13/tools/ipsw-partial.py" "$IPSW_URL" "$OSDMG" <<'E'
import importlib.util, struct, sys
spec = importlib.util.spec_from_file_location("p", sys.argv[1])
p = importlib.util.module_from_spec(spec); spec.loader.exec_module(p)
size, off, n = p.find_eocd(sys.argv[2])
lo, comp, _, method = p.parse_central_directory(p.http_range(sys.argv[2], off, off + n))[sys.argv[3]]
nl, el = struct.unpack_from("<HH", p.http_range(sys.argv[2], lo, lo + 30), 26)
assert method == 0, "the DMG is compressed in the zip"
print(lo + 30 + nl + el, comp)
E
)
    start=${range% *}; total=${range#* }
    touch "$dmg"
    while :; do
        have_=$(wc -c < "$dmg" | tr -d ' ')
        [ "$have_" -ge "$total" ] && break
        a=$((start + have_)); b=$((a + 268435455)); [ "$b" -ge $((start + total)) ] && b=$((start + total - 1))
        printf '\r    downloading the root filesystem: %d / %d MB' $((have_ >> 20)) $((total >> 20))
        if curl -sf --max-time 900 --retry 5 --retry-delay 3 -r "$a-$b" "$IPSW_URL" -o "$tmp/part" \
           && [ "$(wc -c < "$tmp/part" | tr -d ' ')" -eq $((b - a + 1)) ]; then
            cat "$tmp/part" >> "$dmg"
        else
            sleep 5
        fi
    done
    echo
    # the two firmware directories out of the DMG (APFS inside UDIF) with
    # 7-Zip in a container: the same on macOS and Linux, no mounting, no FUSE
    # or root. iOS 16 keeps the Wi-Fi firmware in its DriverKit extension,
    # the Bluetooth firmware in usr/share/firmware/bluetooth.
    mnt="$CACHE/osfw"
    rm -rf "$mnt"
    mkdir -p "$mnt"
    wifi_dir=System/Library/DriverExtensions/com.apple.DriverKit-AppleBCMWLAN.dext/Firmware
    bt_dir=usr/share/firmware/bluetooth
    have docker || fail "docker is needed to read the iOS root filesystem"
    docker run --rm -v "$CACHE":/c alpine:3.24 sh -ec "
        apk add -q --no-cache 7zip >/dev/null 2>&1 || { echo 'apk add 7zip failed' >&2; exit 1; }
        7zz x -y -bso0 -bsp0 -o/c/osfw /c/$OSDMG '$wifi_dir' '$bt_dir'
        chown -R $(id -u):$(id -g) /c/osfw" \
        || fail "could not read the firmware out of $OSDMG"
    wifi="$mnt/$wifi_dir"
    bt="$mnt/$bt_dir"
    unmount() { rm -rf "$mnt"; }
    [ -d "$wifi" ] && [ -d "$bt" ] || { unmount; fail "no Wi-Fi/Bluetooth firmware in $OSDMG"; }
    rm -rf "$INPUTS/brcm"
    # the iPhone 11's module is "moana" (the ADT's wlan module-instance); the
    # Bluetooth table (.ptb) is per antenna: the syscfg's WSKU when known
    ant=R
    if [ -s "$INPUTS/syscfg.bin" ] && python3 -I "$A13/tools/syscfg.py" "$INPUTS/syscfg.bin" get WSKU "$tmp/WSKU" 2>/dev/null; then
        ant=$(dd if="$tmp/WSKU" bs=1 skip=8 count=8 2>/dev/null | tr -d '\0')
    fi
    python3 -I "$A13/tools/brcmfw.py" "$wifi" "$INPUTS" moana >/dev/null
    # the iPhone 11's chip is a B1 (iOS has Bluetooth firmware for B1 only)
    rm -f "$INPUTS/brcm"/brcmfmac4378a* "$INPUTS/brcm"/brcmfmac4378b0*
    python3 -I "$A13/tools/btfw.py" "$bt" "$INPUTS" "${ant:-R}" >/dev/null
    unmount
    note "ok: brcm/ ($(ls "$INPUTS/brcm" | wc -l | tr -d ' ') files: BCM4378 Wi-Fi + Bluetooth, moana, antenna ${ant:-R})"
fi

# ---------- Wi-Fi tools ---------------------------------------------------------
# iw and wpa_supplicant in the image itself: Wi-Fi is how a phone without the
# USB network gets online, so the tools to join cannot wait for apk. Alpine's
# builds with their libraries and their own musl loader under /opt/wifi; the
# bin/ wrappers start them through that loader, so they run in a RAM boot and
# under any root (the `wifi` command finds them there).

say "Wi-Fi tools (iw, wpa_supplicant)"
if [ -x "$A13/build/wifitools/opt/wifi/libexec/wpa_supplicant" ]; then
    note "ok: iw, wpa_supplicant (already there)"
else
    have docker || fail "docker is needed to fetch iw and wpa_supplicant"
    rm -rf "$A13/build/wifitools"
    mkdir -p "$A13/build/wifitools"
    docker run --rm --platform linux/arm64 -e HOST_IDS="$(id -u):$(id -g)" \
        -v "$A13/build/wifitools":/out alpine:3.24 sh -ec '
        apk add -q iw wpa_supplicant >/dev/null
        W=/out/opt/wifi
        mkdir -p $W/bin $W/libexec $W/lib $W/share
        progs="iw wpa_supplicant wpa_cli wpa_passphrase"
        for p in $progs; do
            cp -L "$(command -v $p)" $W/libexec/
            printf "%s\n" "#!/bin/sh" \
                "# $p from Alpine, through its own musl loader (a13/cascadia periphery)" \
                "W=\$(dirname \"\$(dirname \"\$(readlink -f \"\$0\")\")\")" \
                "exec \"\$W/lib/ld-musl-aarch64.so.1\" --library-path \"\$W/lib\" \"\$W/libexec/$p\" \"\$@\"" \
                > $W/bin/$p
        done
        cp -L /lib/ld-musl-aarch64.so.1 $W/lib/
        # one ldd per program: musl ldd reads only its first argument
        for f in $(for p in $progs; do ldd $W/libexec/$p; done 2>/dev/null | sed -n "s/.*=> \([^ ]*\).*/\1/p" | sort -u); do
            cp -L "$f" $W/lib/
        done
        cp -L /usr/share/udhcpc/default.script $W/share/udhcpc.script
        chmod 755 $W/bin/* $W/libexec/* $W/lib/ld-musl-aarch64.so.1 $W/share/udhcpc.script
        chown -R "$HOST_IDS" /out' || fail "could not fetch iw and wpa_supplicant"
    note "ok: iw, wpa_supplicant + their libraries -> build/wifitools"
fi

# ---------- this phone's calibration (syscfg) -------------------------------------
# What iBoot would put into the ADT and this boot chain leaves out: Wi-Fi
# calibration (WCAL), MAC addresses (WMac, BMac), antenna SKU (WSKU), Bluetooth
# calibration (BCAL). NVMe namespace 3, made read-only first. Needs the phone
# booted into Linux once; holds serial numbers, never published.

say "this phone's calibration (syscfg)"
if [ -s "$INPUTS/syscfg.bin" ]; then
    note "ok: syscfg.bin (already there)"
elif ssh -o ConnectTimeout=5 -o BatchMode=yes "$PHONE" \
        'blockdev --setro /dev/nvme0n3 && dd if=/dev/nvme0n3 bs=4096 2>/dev/null' > "$tmp/syscfg" 2>/dev/null \
     && python3 -I "$A13/tools/syscfg.py" "$tmp/syscfg" keys >/dev/null 2>&1; then
    cp "$tmp/syscfg" "$INPUTS/syscfg.bin"
    chmod 600 "$INPUTS/syscfg.bin"
    note "ok: syscfg.bin from $PHONE (private: serial numbers)"
else
    note "not yet: the phone is not reachable at $PHONE -- boot it once and run"
    note "         a13/cascadia periphery again (Wi-Fi then gets its calibration)"
fi

# ---------- internal storage (NVMe) --------------------------------------------
# All of NVMe namespace 1 for Linux, iOS erased: a GPT with one ext4 partition
# "cascadia-root" holding the Alpine root (build/alpine-a13.tar.gz); the phone
# boots from it from then on. Namespaces 2-7 (firmware, syscfg, nvram,
# effaceable) are never written: the NVMe driver's write gate opens namespace
# 1 only, and only in an image packed after the yes here. iOS comes back by a
# restore in Finder / iTunes (the Linux disk goes then). Two runs:
#   1. the yes (type ERASE, or NVME=erase): pack opens the gate; boot
#   2. periphery again, the phone booted from that image: namespaces 2-7 are
#      copied to boot/inputs/nvme-backup/, one more ERASE, then partition,
#      format, unpack Alpine (and the Wi-Fi networks the phone knows)

say "internal storage (NVMe)"
phone() { ssh -o ConnectTimeout=5 -o BatchMode=yes "$PHONE" "$@"; }
erase_ok() {
    ans=${NVME:-}
    if [ -z "$ans" ] && [ -t 0 ]; then
        read -r -p "    $1 " ans || ans=
    fi
    [ "$ans" = ERASE ] || [ "$ans" = erase ]
}
if [ ! -f "$INPUTS/nvme-linux" ]; then
    note "the internal storage holds iOS; Linux can have all of it -- this ERASES"
    note "iOS and everything on the phone (apps, photos, data)"
    if erase_ok "Give the internal storage to Linux? Type ERASE to agree, Enter to keep iOS:"; then
        date > "$INPUTS/nvme-linux"
        note "decided -- next: a13/cascadia pack, boot, then a13/cascadia periphery again"
    else
        note "skipped: iOS stays; the phone runs from NFS or in RAM"
    fi
elif ! phone true 2>/dev/null; then
    note "decided, not done yet: boot the phone (packed after the decision), run periphery again"
elif phone blkid 2>/dev/null | grep -q 'LABEL="cascadia-root"'; then
    note "ok: the Linux disk is there (nvme0n1, cascadia-root)"
elif ! phone 'ls /proc/device-tree/soc/nvme@*/apple,write-ranges' >/dev/null 2>&1; then
    note "the phone runs an image with the NVMe closed: a13/cascadia pack, boot, run periphery again"
else
    bk="$INPUTS/nvme-backup"
    mkdir -p "$bk"
    chmod 700 "$bk"
    for n in 2 3 4 5 6 7; do
        [ -s "$bk/nvme0n$n.bin" ] && continue
        phone "dd if=/dev/nvme0n$n bs=4096 2>/dev/null" > "$bk/nvme0n$n.bin"
        [ -s "$bk/nvme0n$n.bin" ] || fail "could not copy nvme0n$n"
        chmod 600 "$bk/nvme0n$n.bin"
    done
    note "namespaces 2-7 copied to boot/inputs/nvme-backup/ (private: serial numbers)"
    phone 'command -v sgdisk && command -v mkfs.ext4' >/dev/null \
        || fail "the phone has no sgdisk / mkfs.ext4: boot it into Alpine in RAM (a fresh a13/cascadia build rootfs + pack), or: apk add sgdisk e2fsprogs"
    [ -f "$A13/build/alpine-a13.tar.gz" ] || fail "no build/alpine-a13.tar.gz: a13/cascadia build rootfs"
    size=$(phone 'blockdev --getsize64 /dev/nvme0n1')
    if erase_ok "Erase nvme0n1 now ($((size >> 30)) GiB, iOS and all on it)? Type ERASE:"; then
        phone 'set -e
            blockdev --setrw /dev/nvme0n1
            sgdisk --zap-all /dev/nvme0n1 >/dev/null
            sgdisk -n 1:0:0 -t 1:8305 -c 1:cascadia-root /dev/nvme0n1 >/dev/null
            blockdev --rereadpt /dev/nvme0n1
            sleep 1
            mkfs.ext4 -q -F -L cascadia-root /dev/nvme0n1p1' || fail "partitioning / formatting failed (dmesg on the phone)"
        note "nvme0n1: GPT, nvme0n1p1 = ext4 cascadia-root; unpacking Alpine..."
        phone 'set -e
            mkdir -p /mnt/cascadia
            mount -t ext4 /dev/nvme0n1p1 /mnt/cascadia
            tar xzf - -C /mnt/cascadia
            [ -d /etc/wifi ] && cp -a /etc/wifi /mnt/cascadia/etc/
            sync
            umount /mnt/cascadia' < "$A13/build/alpine-a13.tar.gz" || fail "unpacking Alpine failed"
        note "ok: Alpine on the internal storage -- boot the phone (a13/cascadia boot): it starts from it"
    else
        note "not erased"
    fi
fi

say "done -- next: a13/cascadia pack"
