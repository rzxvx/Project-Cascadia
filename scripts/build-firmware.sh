#!/usr/bin/env bash
#
# Build the boot-chain images this port needs, from a stock IPSW.
#
#   iBSS.patched                signature patch only; stock USB receive is left
#                               alone so the host can still upload iBEC
#   iBEC.patched.autogo.dfu     signature patch plus the auto-go hook, packed
#                               the way the stock image is: encrypted, with a
#                               KBAG.  This is the one the checkm8 route sends
#   iBEC.patched.autogo.plain.dfu
#                               the same patched iBEC, packed WITHOUT
#                               encryption.  --kdfu needs this one: after iOS
#                               has booted the AES GID key is gone, so a KBAG
#                               decrypts to nothing and the iBSS jumps into
#                               garbage (scripts/img3pack.py has the detail)
#   P105.mtprops                the digitizer's firmware, out of the root
#                               filesystem (scripts/rootfs-extract.py); touch
#                               needs it and ./cascadia build puts it in the
#                               image
#   brcmfmac4334.bin            the Wi-Fi chip's firmware, wifi/4334b1/borg.trx
#                               from the same place ("borg" is the ADT's
#                               module-instance), a TRX image -- what
#                               brcmfmac downloads over USB -- with the
#                               module's NVRAM appended inside it
#                               (scripts/trx-add-nvram.py): without it the
#                               firmware is downloaded and never comes up
#
# Runs INSIDE the build container (see ./cascadia firmware): the image carries
# pycryptodome, capstone and an iBoot32Patcher built from source, so the result
# is identical on a macOS and a Linux host and nothing outside this repository
# and the user's own IPSW is needed.
#
# Pinned to iOS 8.4.1 / 12H321 and to iPad2,5.  That is not laziness: the auto-go
# hook patches a specific address inside this exact iBEC build
# (scripts/patch-ibec-autogo.py explains which and why), and the decryption keys
# below are that build's.  Another firmware needs both re-derived.
#
# Why an auto-go hook at all, rather than iBoot32Patcher's own -c "go": that
# command runs as soon as it is parsed.  The 64 KB memory probe survived it; a
# 13 MB staging bundle does not -- the loader ran after the first 32 KB chunk,
# before the kernel had landed, which shows up as a magenta screen and no boot.
# The hook fires on the completion whose byte count is short, i.e. at EOF.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IPSW="${IPSW:-$ROOT/ipad25.ipsw}"
OUT="$ROOT/build/firmware"

# Public firmware keys for iPad2,5 / 12H321.  These are not secret: they are
# published in the community key database and are useless without the device.
IBSS_IV="b21abc8689b0dea8f6e613f9f970e241"
IBSS_KEY="b9ed63e4a31f5d9d4d7dddc527e65fd31d1ea48c70204e6b44551c1e6dfc52b5"
IBEC_IV="8460cab6348e74ba7134ba0f9462b632"
IBEC_KEY="485ddb5f7e70cecfc25c036f812641b9e55bd97783de1488306e3a80abf6950b"
ROOTFS_DMG="058-24036-023.dmg"
ROOTFS_KEY="21862ddcc49a861ffda17f7c6eca65355d2d1e762026cca60aabc726cd48b9e4cff214ff"
MTPROPS=/usr/share/firmware/multitouch/P105.mtprops
WIFI_FW=/usr/share/firmware/wifi/4334b1/borg.trx
# The NVRAM goes with the firmware, per module.  The chip names itself
# "s=B3 M=HEIN m=2.6 V=t" (4334B3, TDK), and the iOS 8.4.1 Wi-Fi driver's own
# table, "Heineken - 4334B3 - TDK", picks by the ADT's module-instance: borg
# is borg.trx with borg-t-st.txt (heineken-t-st.txt belongs to centennial.trx,
# the default).  borg.trx with the heineken file came up, and halted on the
# first command -- that file lacks swdiv_en/swdiv_gpio, which a firmware built
# with swdiv wants, and has regrev 9 where borg's has 14.  WIFI_NVRAM= picks
# another file from wifi/4334b1/.
WIFI_NVRAM="${WIFI_NVRAM:-borg-t-st.txt}"
# The module files carry no MAC address -- iOS adds the one in syscfg -- and
# without macaddr= the firmware's wl half never attaches.  Locally administered,
# the same family as the USB gadget's pair in the dts; WIFI_MAC= sets another
# (the iPad's own is under Settings > General > About > Wi-Fi Address).
WIFI_MAC="${WIFI_MAC:-02:10:5a:05:00:03}"

BOOTARGS="${BOOTARGS:-cs_enforcement_disable=1 debug=0x14}"

# Reference hashes, so a regenerated image can be compared rather than trusted.
#
# The iBEC one is of the PLAINTEXT, before it is repacked into an img3, and that
# is deliberate.  The .dfu that this device has actually been booting is a
# binary preserved back in August by restore-known-good.sh, and the auto-go
# script has changed since: its trampoline now differs from the one in that
# file, starting two bytes in.  Everything downstream of that differs too,
# because the img3 payload is AES-CBC and a single changed block poisons the
# rest -- which is why comparing the .dfu says nothing useful about whether the
# pipeline is correct.  The plaintext hash does: it is what the author's own
# current scripts produce, verified byte-for-byte.
#
# The .dfu this produces is not byte-identical to the preserved August image,
# and that is expected rather than a problem: the auto-go script changed in
# between.  This build has since been flashed and the device booted from it, so
# it is the tested one now.
KNOWN_IBEC_PLAIN_MD5="b7e502c0262660b68adac4fe4e764b1b"
KNOWN_IBSS_MD5="8b6dcc510c0ab303d67495978f4eb523"
# The same bytes iOS itself has at that path on the iPad (read there over ssh
# and compared).
KNOWN_MTPROPS_MD5="807fdbc8816df68cf5d57a3964bd8400"

fail() { echo "error: $*" >&2; exit 1; }
md5of() { md5sum "$1" | cut -d' ' -f1; }

[ -f "$IPSW" ] || fail "no IPSW at $IPSW -- pass IPSW=/path/to/iPad2,5_8.4.1_12H321_Restore.ipsw"

PATCHER="${PATCHER:-/usr/local/bin/iBoot32Patcher}"
command -v "$PATCHER" >/dev/null 2>&1 || [ -x "$PATCHER" ] \
    || fail "no iBoot32Patcher at $PATCHER -- rebuild the image: docker rmi cascadia-build"

mkdir -p "$OUT"

build_one() {
    local name="$1" iv="$2" key="$3"
    echo "==> $name: extract, decrypt, patch"
    unzip -o -j "$IPSW" "Firmware/dfu/${name}.p105.RELEASE.dfu" -d "$OUT" >/dev/null \
        || fail "$name not found in $IPSW -- is this an iPad2,5 8.4.1 IPSW?"
    python3 "$ROOT/scripts/img3decrypt.py" \
        "$OUT/${name}.p105.RELEASE.dfu" "$OUT/${name}.dec" "$iv" "$key"
    "$PATCHER" "$OUT/${name}.dec" "$OUT/${name}.patched" -b "$BOOTARGS" >/dev/null
}

build_one iBSS "$IBSS_IV" "$IBSS_KEY"
# Signature patch only.  The park hooks that once lived here rebooted the device
# straight back into iOS, and iBSS has to keep its stock USB receive path or
# there is no way to upload iBEC over the cable.
python3 "$ROOT/scripts/verify-ibss-clean.py" "$OUT/iBSS.patched"

build_one iBEC "$IBEC_IV" "$IBEC_KEY"
echo "==> iBEC: auto-go hook"
python3 "$ROOT/scripts/patch-ibec-autogo.py" "$OUT/iBEC.patched" "$OUT/iBEC.autogo"
echo "==> iBEC: repack as img3 (encrypted -- the checkm8 route)"
python3 "$ROOT/scripts/img3encrypt.py" \
    "$OUT/iBEC.p105.RELEASE.dfu" "$OUT/iBEC.autogo" \
    "$OUT/iBEC.patched.autogo.dfu" "$IBEC_IV" "$IBEC_KEY"
echo "==> iBEC: repack as img3 (plaintext -- the kDFU route)"
python3 "$ROOT/scripts/img3pack.py" \
    "$OUT/iBEC.p105.RELEASE.dfu" "$OUT/iBEC.autogo" \
    "$OUT/iBEC.patched.autogo.plain.dfu"

echo "==> touch and Wi-Fi firmware out of the root filesystem"
python3 "$ROOT/scripts/rootfs-extract.py" "$IPSW" "$ROOTFS_DMG" "$ROOTFS_KEY" \
    "$MTPROPS" "$OUT/P105.mtprops" "$WIFI_FW" "$OUT/borg.trx" \
    "/usr/share/firmware/wifi/4334b1/$WIFI_NVRAM" "$OUT/brcmfmac4334-nvram.txt"
python3 "$ROOT/scripts/trx-add-nvram.py" "$OUT/borg.trx" "$OUT/brcmfmac4334-nvram.txt" \
    "$OUT/brcmfmac4334.bin" "$WIFI_MAC"

echo
rc=0
for pair in "iBSS.patched:$KNOWN_IBSS_MD5" "iBEC.autogo:$KNOWN_IBEC_PLAIN_MD5" "P105.mtprops:$KNOWN_MTPROPS_MD5"; do
    f=${pair%%:*}; want=${pair##*:}; got=$(md5of "$OUT/$f")
    if [ "$got" = "$want" ]; then
        echo "    ok: $f reproduces the reference byte for byte"
    else
        echo "    MISMATCH: $f is $got, expected $want"
        rc=1
    fi
done
if [ "$rc" -ne 0 ]; then
    cat <<'EOF'

    Something in the chain changed.  The decrypted input, iBoot32Patcher's
    output and the auto-go patch are each deterministic, so a mismatch here
    points at one of them rather than at noise.
EOF
else
    cat <<'EOF'

    This differs from the preserved August .dfu, because the auto-go script
    changed after that image was saved.  It has been flashed and the device
    booted from it.  ./cascadia flash --known-good still sends the old one if
    you ever need to tell a bad build from a bad bench.
EOF
fi

echo
ls -l "$OUT/iBSS.patched" "$OUT/iBEC.patched.autogo.dfu" \
      "$OUT/iBEC.patched.autogo.plain.dfu" "$OUT/P105.mtprops" "$OUT/brcmfmac4334.bin"
