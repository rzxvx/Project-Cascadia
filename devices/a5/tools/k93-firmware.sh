#!/usr/bin/env bash
#
# The iPad 2's Apple files -- what ./cascadia k93 firmware runs.
#
#   bash tools/k93-firmware.sh             the boot chain, and the iPad's own
#                                          files if its iOS answers over USB
#   K93_FROM_IOS=0 bash tools/k93-firmware.sh   the boot chain only
#
# Into build/k93/firmware/:
#
#   iBSS.patched                  12H321's iBSS, signature checks off, raw:
#                                 what kloader boots (tools/kdfu.sh)
#   iBEC.patched.autogo.plain.dfu 12H321's iBEC, signature checks off and the
#                                 auto-go hook, a plaintext img3: kDFU leaves
#                                 the GID key locked, so it cannot have a KBAG
#   K93.mtprops                   the digitizer's firmware (Z2F13,2)
#   brcm/brcmfmac4329-sdio.bin    the BCM4329's Wi-Fi firmware, as iOS has it
#                                 (wifi/4329c0/second.bin)
#   brcm/brcmfmac4329-sdio.txt    its NVRAM (second.txt) with this iPad's MAC
#                                 added: without macaddr= the firmware never
#                                 brings wl up (the mini's lesson, README)
#   brcm/BCM4329C0.hcd            the Bluetooth patchram, out of BlueTool
#   apple/sgx543.fw               the GPU's microkernel, out of the IPSW's kernelcache
#   apple/h4isp-firmware.bin      the camera ISP's firmware and its sensors'
#   apple/h4isp-setfile-*.bin     set-files, out of the IPSW's root filesystem
#                                 (scripts/extract-h4isp-firmware.py)
#
# and this iPad's touch calibration into build/k93/keep/lib/firmware/mtcal.bin.
#
# The boot chain comes from iPad2,1_8.4.1_12H321_Restore.ipsw (offered for
# download, Apple's own URL) by tools/k93-extract-firmware.py and
# tools/k93-build-bootchain.py, with the public 12H321 keys, and so do the
# GPU's and the cameras' files.  The rest comes from the iPad's own iOS over
# ssh, read only: iOS 6.1.3's files are the ones this iPad runs.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="$(cd "$ROOT/../.." && pwd)"
OUT="$ROOT/build/k93/firmware"
KEEP="$ROOT/build/k93/keep/lib/firmware"
IPSW_NAME="iPad2,1_8.4.1_12H321_Restore.ipsw"
IPSW_URL="https://secure-appldnld.apple.com/ios8.4.1/031-31231-20150812-751BEF2C-3C8F-11E5-94D1-BB1A3A53DB92/$IPSW_NAME"
IPSW_SHA1=529fc75ec834111e255a74241eeb14fb7cc5c46b
# The module in this iPad: BlueTool's table puts the "2ND" patchram with the
# BCM4329C0, whose Wi-Fi files are wifi/4329c0/second.* -- the only ones
# iOS 6.1.3 installs on the K93.  BT_MODULE= picks another image.
BT_MODULE="${BT_MODULE:-Olympic Second Module}"

say()  { printf '==> %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }

mkdir -p "$OUT/brcm" "$KEEP"

# ---------------------------------------------------------------- IPSW ----
ipsw=
for c in "${IPSW:-}" "$ROOT/$IPSW_NAME" "$REPO/$IPSW_NAME"; do
    [ -n "$c" ] && [ -f "$c" ] && { ipsw=$c; break; }
done
if [ -z "$ipsw" ]; then
    echo "No $IPSW_NAME here.  The iPad 2's boot chain comes out of it (about 1.4 GB,"
    echo "Apple's own server):  $IPSW_URL"
    ans=n
    if [ -t 0 ]; then read -r -p "Download it now into $ROOT? [Y/n] " ans || ans=n; ans=${ans:-y}; fi
    case "$ans" in y|Y|yes) ;; *) fail "no IPSW (or IPSW=/path/to/it)";; esac
    curl -fL -C - --retry 3 -o "$ROOT/$IPSW_NAME.part" "$IPSW_URL" || fail "the download stopped; run this again to resume"
    echo "$IPSW_SHA1  $ROOT/$IPSW_NAME.part" | sha1sum -c --quiet - || fail "$IPSW_NAME.part does not match Apple's SHA-1; delete it"
    mv "$ROOT/$IPSW_NAME.part" "$ROOT/$IPSW_NAME"
    ipsw="$ROOT/$IPSW_NAME"
fi
say "IPSW: $ipsw"

# ---------------------------------------------------------- boot chain ----
# Both scripts in the build image: iBoot32Patcher, pycryptodome and capstone
# are there.  The IPSW is mounted read-only at the name the extractor wants.
say "the boot chain (iBSS, iBEC) out of the IPSW"
dkr=(docker run --rm)
[ "$(uname -s)" = Linux ] && dkr+=(--user "$(id -u):$(id -g)")
"${dkr[@]}" -v "$ROOT":/cascadia -v "$ipsw":/ipsw.ipsw:ro -w /cascadia "${IMAGE:-cascadia-build}" \
    bash -c "python3 tools/k93-extract-firmware.py /ipsw.ipsw && python3 tools/k93-build-bootchain.py && mkdir -p build/k93/firmware/apple && python3 scripts/extract-sgx-firmware.py --board k93 /ipsw.ipsw build/k93/firmware/apple/sgx543.fw && python3 scripts/extract-h4isp-firmware.py /ipsw.ipsw build/k93/firmware/apple" \
    | grep -v '^    0x' || fail "the boot chain did not build (above)"
BC="$ROOT/build/k93/bootchain-v2"
cp "$BC/iBSS.sigpatched" "$OUT/iBSS.patched"
cp "$BC/iBEC.autogo.plain.dfu" "$OUT/iBEC.patched.autogo.plain.dfu"
[ "$(od -An -tx1 -N4 "$OUT/iBSS.patched" | tr -d ' \n')" = 0e0000ea ] || fail "iBSS.patched is not a raw iBSS"
note "ok: iBSS.patched, iBEC.patched.autogo.plain.dfu"

# ----------------------------------------------------- the iPad's iOS ----
if [ "${K93_FROM_IOS:-1}" = 0 ]; then
    note "K93_FROM_IOS=0: not asking the iPad for touch, Wi-Fi and Bluetooth files"
    exit 0
fi
. "$ROOT/tools/ios-ssh.sh"
ios_setup k93fw
if ! ios_connect; then
    note "no ssh to the iPad's iOS: the touch, Wi-Fi and Bluetooth files are not fetched."
    note "Boot it into its jailbroken iOS, on the cable, and run this again."
    exit 0
fi
model=$(ios uname -m)
[ "$model" = iPad2,1 ] || fail "this is a $model, not an iPad2,1"
say "touch, Wi-Fi and Bluetooth files from the iPad's iOS ($(ios sw_vers -productVersion 2>/dev/null || echo iOS))"
get() { ios "cat '$1'" > "$2" && [ -s "$2" ] || fail "could not read $1 on the iPad"; }
get /usr/share/firmware/multitouch/K93.mtprops "$OUT/K93.mtprops"
get /usr/share/firmware/wifi/4329c0/second.bin "$OUT/brcm/brcmfmac4329-sdio.bin"
get /usr/share/firmware/wifi/4329c0/second.txt "$WORK/second.txt"
get /usr/sbin/BlueTool "$WORK/BlueTool"

# The MAC: locally administered by default, in the family of the gadget's
# pair (dts/k93ap.dts), as the mini's build does; WIFI_MAC=own puts in this
# iPad's own (lockdown's WiFiAddress, ideviceinfo), WIFI_MAC=xx:.. any other.
mac="${WIFI_MAC:-02:93:00:00:00:03}"
if [ "$mac" = own ]; then
    mac=$(ideviceinfo -k WiFiAddress 2>/dev/null || true)
    [ -n "$mac" ] || fail "WIFI_MAC=own, but ideviceinfo could not read the iPad's Wi-Fi address"
fi
{ grep -v '^macaddr=' "$WORK/second.txt"; echo "macaddr=$mac"; } > "$OUT/brcm/brcmfmac4329-sdio.txt"
note "ok: Wi-Fi firmware and NVRAM (macaddr=$mac)"

python3 "$ROOT/scripts/extract-bt-firmware.py" "$WORK/BlueTool" "$BT_MODULE" "$OUT/brcm/BCM4329C0.hcd" \
    || fail "no Bluetooth patchram for $BT_MODULE in BlueTool"

# The panel's calibration: iBoot copies syscfg's MtCl into the device tree,
# and iOS is the only place to read it (tools/mtdump/mtcal.c, prebuilt).
ios 'rm -f /tmp/mtcal && cat > /tmp/mtcal && chmod 755 /tmp/mtcal' < "$ROOT/tools/mtdump/prebuilt/mtcal"
ios /tmp/mtcal > "$WORK/mtcal.bin" || fail "mtcal failed on the iPad (above)"
ios rm -f /tmp/mtcal
n=$(wc -c < "$WORK/mtcal.bin" | tr -d ' ')
[ "$n" -gt 0 ] && od -An -tx1 "$WORK/mtcal.bin" | tr -d ' \n' | grep -q '[1-9a-f]' || fail "the calibration is empty"
cp "$WORK/mtcal.bin" "$KEEP/mtcal.bin"
note "ok: touch calibration, $n bytes (build/k93/keep/lib/firmware/mtcal.bin)"
ls -l "$OUT" "$OUT/brcm"
