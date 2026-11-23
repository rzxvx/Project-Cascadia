#!/usr/bin/env bash
# firmware.sh - the iPad 7's firmware that comes from Apple's IPSW, read from
# Apple's CDN (partial reads: the kernelcache, not the whole IPSW).
#
# The reference build is iPadOS 18.7.9 (22H355, one IPSW for iPad7,11 and
# iPad7,12), the one the kernel's drivers were validated with (Pauli1Go's
# HoolockLinux-linux-firmware, ipad7.md). Every source and output is checked
# against the sizes and SHA-256 sums given there.
#
#   apple/t8010-smartio.bin   the SmartIO coprocessor (SPI DMA for the touch
#                             controller): AppleT8010SmartIO's __DATA.__firmware
#   apple/dfrmtfw-j172-k1f19-6.bin
#                             the touch controller's firmware with THIS iPad's
#                             calibration: J17x_Multitouch.im4p (one image for
#                             J171 and J172) + build/private/syscfg.bin
#                             (./cascadia a10 syscfg), through makez2fw. The
#                             name is the one the shared iPad 7 DT asks for.
#                             Private: it carries this iPad's calibration.
#   brcm/brcmfmac4355c1-pcie.apple,rudderb*.{bin,txt,clm_blob,txcap_blob}
#                             Wi-Fi (BCM4355C1, the Murata/USI "rudderb"
#                             module), named the six ways Pauli1Go's guide
#                             stages them; brcmfmac picks by the module's OTP
#   brcm/BCM4355C1.hcd        Bluetooth: the Patchram image hcdpack cuts out
#                             of iPadOS's /usr/sbin/BlueTool
#   regulatory.db{,.p7s}      cfg80211's, from Alpine's wireless-regdb
#                             The Wi-Fi and Bluetooth files live in the root
#                             filesystem image (5.9 GB, AEA-encrypted): it is
#                             downloaded once (resumable, build/fwsrc), decrypted
#                             (A10_OSDMG_KEY), the two places read out with
#                             7-Zip in a container, the image deleted again.
#
# Output: devices/a10/build/firmware/<path under /lib/firmware>; pack puts
# the tree into the initramfs.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
A10="$(cd "$HERE/.." && pwd)"
SRC="$A10/build/fwsrc"
FW="$A10/build/firmware"
mkdir -p "$SRC" "$FW/apple"

BUILD=22H355
KC="$SRC/${BUILD}__iPad7,11/kernelcache.release.iPad7,11_12"
KC_SIZE=48529408
KC_SHA=f0c322154b4927f230b0ed1d06ec445872d6b809c1a0d2acbef36e177c5379e3
SIO_OFF=$((0x02a46000))
SIO_SIZE=$((0x00088060))
SIO_SHA=71d274a106f5912ed33552ba75ae76cc5081fc7063935ec938f9cb503df38199

sha() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
check() {   # file size sha
    [ "$(wc -c < "$1" | tr -d ' ')" = "$2" ] || { echo "error: $1: size is not $2" >&2; exit 1; }
    [ "$(sha "$1")" = "$3" ] || { echo "error: $1: SHA-256 mismatch" >&2; exit 1; }
}

if [ ! -f "$KC" ]; then
    echo "==> kernelcache of iPadOS 18.7.9 ($BUILD) from Apple's CDN"
    ( cd "$SRC" && ipsw download ipsw --build "$BUILD" -d iPad7,11 -y --kernel >/dev/null )
fi
check "$KC" "$KC_SIZE" "$KC_SHA"

echo "==> apple/t8010-smartio.bin"
dd if="$KC" of="$FW/apple/t8010-smartio.bin" bs=4096 \
    skip=$((SIO_OFF / 4096)) count=$(((SIO_SIZE + 4095) / 4096)) 2>/dev/null
truncate -s "$SIO_SIZE" "$FW/apple/t8010-smartio.bin"
check "$FW/apple/t8010-smartio.bin" "$SIO_SIZE" "$SIO_SHA"

# touch: needs this iPad's SysCfg
MT="$SRC/${BUILD}__iPad7,11/Firmware/J171_Multitouch.im4p"
MT_SIZE=2205551
MT_SHA=159f75dbbbf694a47af0fa16d00f2cbf3cf9a3dd34bfdb8da5db1d167a3e9889
MTFW_SIZE=2205526
MTFW_SHA=cb67cae5956af0ead9f4ffbc2a5e6f2ffdfea667db51e379d24fd3b6713ffbdf
SYSCFG="$A10/build/private/syscfg.bin"
if [ -f "$SYSCFG" ]; then
    [ -x "$A10/build/bin/makez2fw" ] || { echo "error: no makez2fw -- run: ./cascadia a10 build tools" >&2; exit 1; }
    if [ ! -f "$MT" ]; then
        echo "==> J171_Multitouch.im4p from Apple's CDN"
        ( cd "$SRC" && ipsw download ipsw --build "$BUILD" -d iPad7,11 -y \
              --pattern '^Firmware/J171_Multitouch\.im4p$' >/dev/null )
    fi
    check "$MT" "$MT_SIZE" "$MT_SHA"
    ipsw img4 im4p extract --output "$SRC/J171_Multitouch.mtfw" "$MT" >/dev/null
    check "$SRC/J171_Multitouch.mtfw" "$MTFW_SIZE" "$MTFW_SHA"
    echo "==> apple/dfrmtfw-j172-k1f19-6.bin (with this iPad's calibration)"
    "$A10/build/bin/makez2fw" 'K1F19,6' "$SRC/J171_Multitouch.mtfw" "$SYSCFG" \
        "$FW/apple/dfrmtfw-j172-k1f19-6.bin"
    chmod 600 "$FW/apple/dfrmtfw-j172-k1f19-6.bin"
else
    echo "note: no build/private/syscfg.bin -- no touch firmware (boot Linux, then: ./cascadia a10 syscfg)"
fi

# ---------- Wi-Fi and Bluetooth: out of the root filesystem image ----------
. "$A10/sources.env"
OSFW="$SRC/osfw"
WIFI_DIR=System/Library/DriverExtensions/com.apple.DriverKit-AppleBCMWLAN.dext/Firmware/C-4355__s-C1
BLUETOOL=usr/sbin/BlueTool
BLUETOOL_SIZE=5221024
BLUETOOL_SHA=dadd9b2086cbc98bb498e4899ce42303a3a03fe0509955bf2de583447532b0e0
HCD_SIZE=76184
HCD_SHA=27b778a87f086d8c23f0ad06c77e4b62de9af83182f100ddf4a6e59b1d4249b7
if [ ! -f "$OSFW/$BLUETOOL" ]; then
    AEA="$SRC/$A10_OSDMG"
    range=$(python3 -I - "$A10/../a13/tools/ipsw-partial.py" "$A10_IPSW_URL" "$A10_OSDMG" <<'E'
import importlib.util, struct, sys
spec = importlib.util.spec_from_file_location("p", sys.argv[1])
p = importlib.util.module_from_spec(spec); spec.loader.exec_module(p)
size, off, n = p.find_eocd(sys.argv[2])
lo, comp, _, method = p.parse_central_directory(p.http_range(sys.argv[2], off, off + n))[sys.argv[3]]
nl, el = struct.unpack_from("<HH", p.http_range(sys.argv[2], lo, lo + 30), 26)
assert method == 0, "the image is compressed in the zip"
print(lo + 30 + nl + el, comp)
E
)
    start=${range% *}; total=${range#* }
    touch "$AEA"
    while :; do
        have_=$(wc -c < "$AEA" | tr -d ' ')
        [ "$have_" -ge "$total" ] && break
        a=$((start + have_)); b=$((a + 268435455)); [ "$b" -ge $((start + total)) ] && b=$((start + total - 1))
        printf '\r==> the root filesystem image: %d / %d MB' $((have_ >> 20)) $((total >> 20))
        if curl -sf --max-time 900 --retry 5 --retry-delay 3 -r "$a-$b" "$A10_IPSW_URL" -o "$SRC/part" \
           && [ "$(wc -c < "$SRC/part" | tr -d ' ')" -eq $((b - a + 1)) ]; then
            cat "$SRC/part" >> "$AEA"
        else
            sleep 5
        fi
    done
    rm -f "$SRC/part"; echo
    echo "==> decrypting it (AEA)"
    if command -v aea >/dev/null; then
        aea decrypt -i "$AEA" -o "$SRC/os.apfs" -key-value "base64:$A10_OSDMG_KEY"
    else
        ipsw fw aea --key-val "$A10_OSDMG_KEY" --output "$SRC" "$AEA" && mv "$SRC/${A10_OSDMG%.aea}" "$SRC/os.apfs"
    fi
    echo "==> reading the firmware out of it (7-Zip, in a container)"
    rm -rf "$OSFW"; mkdir -p "$OSFW"
    docker run --rm -v "$SRC":/c alpine:3.24 sh -ec "
        apk add -q --no-cache 7zip >/dev/null
        7zz x -y -bso0 -bsp0 -tapfs -o/c/osfw /c/os.apfs '$WIFI_DIR' '$BLUETOOL'
        chown -R $(id -u):$(id -g) /c/osfw"
    rm -f "$SRC/os.apfs"
    echo "    (the encrypted image stays in build/fwsrc/$A10_OSDMG; delete it when done)"
fi
check "$OSFW/$BLUETOOL" "$BLUETOOL_SIZE" "$BLUETOOL_SHA"

echo "==> brcm/: Wi-Fi (rudderb)"
mkdir -p "$FW/brcm"
python3 -I - "$OSFW/$WIFI_DIR" "$FW/brcm" <<'E'
import hashlib, os, sys
src, out = sys.argv[1], sys.argv[2]
want = {"rudderb.trx": (732185, "29276526471d794c59646b0ba89c31c2e5c48b88435412f22e3cfee32c8d441d"),
        "rudderb.clmb": (9097, "bfe78741f0cb1e80aea37cd99727af2adef8e9357bfe02fa4d7a278f6a088d87"),
        "rudderb.txcb": (602, "3fc2d256403ed4b429e160dc83990092402098a82003a0bc241282ce3cd7fcee"),
        "P-rudderb_M-YSBU_V-m__m-2.5.txt": (5386, "23d0be41c025c935db90b95143c62ef2203e5e7543c20a1bfadefd56fd5b68a1"),
        "P-rudderb_M-YSBU_V-u__m-4.3.txt": (5396, "26fa336993183f93f342a2294326b9be51cc57f95f65a70dc48a44daefa66a67")}
data = {}
for name, (size, sha) in want.items():
    b = open(os.path.join(src, name), "rb").read()
    if len(b) != size or hashlib.sha256(b).hexdigest() != sha:
        raise SystemExit(f"error: {name}: size or SHA-256 not the reference's")
    data[name] = b
def nvram(b):   # brcmfmac's NVRAM: key=value lines
    return "".join(f"{k.strip()}={v}\n" for k, v in (l.split("=", 1) for l in b.decode("ascii").split("\n") if l)).encode()
nv = {"m": nvram(data["P-rudderb_M-YSBU_V-m__m-2.5.txt"]), "u": nvram(data["P-rudderb_M-YSBU_V-u__m-4.3.txt"])}
# the six names the guide stages: the plain fallback takes the m-2.5 profile
boards = {"rudderb": "m", "rudderb-XX": "m",
          "rudderb-YSBU-m-2.5": "m", "rudderb-YSBU-m-2.5-XX": "m",
          "rudderb-YSBU-u-4.3": "u", "rudderb-YSBU-u-4.3-XX": "u"}
for board, prof in boards.items():
    base = os.path.join(out, f"brcmfmac4355c1-pcie.apple,{board}")
    for ext, b in (("bin", data["rudderb.trx"]), ("clm_blob", data["rudderb.clmb"]),
                   ("txcap_blob", data["rudderb.txcb"]), ("txt", nv[prof])):
        open(f"{base}.{ext}", "wb").write(b)
print(f"    {6 * 4} files")
E

echo "==> brcm/BCM4355C1.hcd: Bluetooth"
"$A10/build/bin/hcdpack" "$OSFW/$BLUETOOL" 'C-4355__s-C1' rudderB 'M-YSBU_V-m__m-2.5' "$FW/brcm/BCM4355C1.hcd" >/dev/null
check "$FW/brcm/BCM4355C1.hcd" "$HCD_SIZE" "$HCD_SHA"

echo "==> regulatory.db (wireless-regdb)"
docker run --rm --platform linux/arm64 -v "$FW":/fw alpine:3.24 sh -ec '
    apk add -q wireless-regdb >/dev/null
    cp /lib/firmware/regulatory.db /lib/firmware/regulatory.db.p7s /fw/'

echo "firmware OK: $FW"
