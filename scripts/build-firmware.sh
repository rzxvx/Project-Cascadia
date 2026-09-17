#!/usr/bin/env bash
#
# Build the two boot-chain images this port needs, from a stock IPSW.
#
#   iBSS.patched                signature patch only; stock USB receive is left
#                               alone so the host can still upload iBEC
#   iBEC.patched.autogo.dfu     signature patch plus the auto-go hook
#
# Runs INSIDE the build container (see ./cascadia firmware), because it needs
# pycryptodome and capstone, and because using Legacy iOS Kit's Linux
# iBoot32Patcher there makes the result identical on a macOS and a Linux host.
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
LIK="${LIK:-/lik}"
OUT="$ROOT/build/firmware"

# Public firmware keys for iPad2,5 / 12H321.  These are not secret: they are
# published in the community key database and are useless without the device.
IBSS_IV="b21abc8689b0dea8f6e613f9f970e241"
IBSS_KEY="b9ed63e4a31f5d9d4d7dddc527e65fd31d1ea48c70204e6b44551c1e6dfc52b5"
IBEC_IV="8460cab6348e74ba7134ba0f9462b632"
IBEC_KEY="485ddb5f7e70cecfc25c036f812641b9e55bd97783de1488306e3a80abf6950b"

BOOTARGS="${BOOTARGS:-cs_enforcement_disable=1 debug=0x14}"

# The images that are known to boot this device, so a regenerated one can be
# compared rather than trusted.  A mismatch is not automatically wrong -- a
# different iBoot32Patcher build reorders padding -- but it is always worth
# knowing before the device is asked to run it.
KNOWN_IBEC_MD5="1937140671116d058713914d603585e6"
KNOWN_IBSS_MD5="8b6dcc510c0ab303d67495978f4eb523"

fail() { echo "error: $*" >&2; exit 1; }
md5of() { md5sum "$1" | cut -d' ' -f1; }

[ -f "$IPSW" ] || fail "no IPSW at $IPSW -- pass IPSW=/path/to/iPad2,5_8.4.1_12H321_Restore.ipsw"

ARCH=$(uname -m); case "$ARCH" in aarch64|arm64) LARCH=arm64 ;; *) LARCH=x86_64 ;; esac
PATCHER="$LIK/bin/linux/$LARCH/iBoot32Patcher"
[ -x "$PATCHER" ] || fail "no iBoot32Patcher at $PATCHER
Legacy iOS Kit provides it.  Clone it and point ./cascadia at it:
    git clone https://github.com/LukeZGD/Legacy-iOS-Kit.git ~/Legacy-iOS-Kit"

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
echo "==> iBEC: repack as img3"
python3 "$ROOT/scripts/img3encrypt.py" \
    "$OUT/iBEC.p105.RELEASE.dfu" "$OUT/iBEC.autogo" \
    "$OUT/iBEC.patched.autogo.dfu" "$IBEC_IV" "$IBEC_KEY"

echo
rc=0
for pair in "iBSS.patched:$KNOWN_IBSS_MD5" "iBEC.patched.autogo.dfu:$KNOWN_IBEC_MD5"; do
    f=${pair%%:*}; want=${pair##*:}; got=$(md5of "$OUT/$f")
    if [ "$got" = "$want" ]; then
        echo "    ok: $f matches the image known to boot this device"
    else
        echo "    NOTE: $f is $got, known-good is $want"
        rc=1
    fi
done
[ "$rc" -eq 0 ] || cat <<'EOF'

    A mismatch is not proof of breakage -- a different iBoot32Patcher build can
    reorder padding without changing behaviour -- but nothing here has been run
    on hardware yet.  Flash it knowing that, and keep a known-good copy.
EOF

echo
ls -l "$OUT/iBSS.patched" "$OUT/iBEC.patched.autogo.dfu"
