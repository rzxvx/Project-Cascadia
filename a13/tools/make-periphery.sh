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
#
# Output (boot/inputs/, not tracked): n104-mtfw.bin, n104-mtcal.bin, brcm/,
# syscfg.bin; build/fbkeyboard/, build/wifitools/ (small trees for the
# initramfs).
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
            docker run --rm --platform linux/arm64 -v "$A13/build/fbkeyboard":/out alpine:3.24 sh -ec '
                apk add -q fbkeyboard font-dejavu >/dev/null
                for f in /usr/bin/fbkeyboard /lib/ld-musl-aarch64.so.1 \
                         $(ldd /usr/bin/fbkeyboard | sed -n "s/.*=> \([^ ]*\).*/\1/p") \
                         /usr/share/fonts/dejavu/DejaVuSans.ttf; do
                    mkdir -p "/out$(dirname "$f")"
                    cp -L "$f" "/out$f"
                done' || fail "could not fetch fbkeyboard"
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
    mnt="$tmp/os"
    mkdir -p "$mnt"
    case "$(uname -s)" in
        Darwin) hdiutil attach -readonly -nobrowse -noverify -mountpoint "$mnt" "$dmg" >/dev/null \
                    || fail "could not mount $dmg" ;;
        *)      have apfs-fuse || fail "mounting the iOS root filesystem on Linux needs apfs-fuse"
                apfs-fuse "$dmg" "$mnt" || fail "could not mount $dmg"
                mnt="$mnt/root" ;;
    esac
    # iOS 16 keeps the Wi-Fi firmware in its DriverKit extension, the
    # Bluetooth firmware in usr/share/firmware/bluetooth
    wifi="$mnt/System/Library/DriverExtensions/com.apple.DriverKit-AppleBCMWLAN.dext/Firmware"
    bt="$mnt/usr/share/firmware/bluetooth"
    unmount() { case "$(uname -s)" in Darwin) hdiutil detach "$tmp/os" >/dev/null ;; *) fusermount -u "$tmp/os" ;; esac; }
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
    docker run --rm --platform linux/arm64 -v "$A13/build/wifitools":/out alpine:3.24 sh -ec '
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
        chmod 755 $W/bin/* $W/libexec/* $W/lib/ld-musl-aarch64.so.1 $W/share/udhcpc.script' || fail "could not fetch iw and wpa_supplicant"
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

say "done -- next: a13/cascadia pack"
