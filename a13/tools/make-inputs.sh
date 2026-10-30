#!/usr/bin/env bash
#
# make-inputs.sh - build the four boot inputs from Apple's CDN.
#
# Downloads the iBSS and DeviceTree from the 20E247 IPSW (Apple's server),
# decrypts the iBSS with its public key, patches the one signature-check
# instruction, wraps the DeviceTree in a restore IMG4, and creates an empty
# trust cache.  Nothing from Apple is shipped; the user's machine fetches and
# transforms them.
#
# Needs: ipsw (3.1.714 tested), python3.
# Output: a13/boot/inputs/{iBSS.n104.20E247.sigpatched.raw, IM4M,
#         DeviceTree.n104.rdtr.img4, empty.rtsc.img4}
#
# The decryption key is from pwnerblu/surrealra1n (public, MIT-licensed).
# The sig-check patch is a single instruction at a known offset in the
# 20E247 iBSS (documented in boot/inputs/README.md).
set -euo pipefail

A13="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INPUTS="$A13/boot/inputs"
mkdir -p "$INPUTS"

say()  { printf '==> %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

have ipsw  || fail "ipsw not found (https://github.com/blacktop/ipsw)"
have python3 || fail "python3 not found"

BUILD=20E247
DEVICE=iPhone12,1

# iBSS AES key (IV + Key), from pwnerblu/surrealra1n keys/iPhone12,1.txt
IBSS_IV=98df294debc43c1488cb4c563f5d3b1f
IBSS_KEY=6aaddc1c0a47f9e149ca175cc7eb02e7af4225489093aee6770f61f136f587ef

# Expected checksums (from boot/inputs/README.md)
IBSS_SHA256=5b77df1fceb136c42ced74003f6d8b5b1acacb56bda5f88d112895e4853034f2
DT_SHA256=62b7e44eb0b3f220124283d012cacb7cfdef259444c6cf8e0539ad255dafe9e2

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# ---------- 1. download from Apple's CDN ------------------------------------

say "downloading iBSS and DeviceTree from Apple (build $BUILD)"
ipsw download ipsw --device "$DEVICE" --build "$BUILD" \
    --pattern 'Firmware/dfu/iBSS.n104.RELEASE.im4p' \
    -o "$tmp" --confirm
ipsw download ipsw --device "$DEVICE" --build "$BUILD" \
    --pattern 'Firmware/all_flash/DeviceTree.n104ap.im4p' \
    -o "$tmp" --confirm

IPSW_DIR="$tmp/${BUILD}__${DEVICE}"
IBSS_IM4P="$IPSW_DIR/Firmware/dfu/iBSS.n104.RELEASE.im4p"
DT_IM4P="$IPSW_DIR/Firmware/all_flash/DeviceTree.n104ap.im4p"
[ -f "$IBSS_IM4P" ] || fail "iBSS download failed"
[ -f "$DT_IM4P" ]   || fail "DeviceTree download failed"

# ---------- 2. iBSS: decrypt, patch, verify ---------------------------------

say "decrypting iBSS"
ipsw img4 im4p extract --iv "$IBSS_IV" --key "$IBSS_KEY" \
    "$IBSS_IM4P" -o "$tmp/iBSS.raw"

say "patching signature check (offset 0x293d8)"
python3 -c "
import sys
data = bytearray(open(sys.argv[1], 'rb').read())
off = 0x293d8
old = bytes.fromhex('e00314aa')
new = bytes.fromhex('000080d2')
if data[off:off+4] != old:
    print(f'error: expected {old.hex()} at 0x{off:x}, got {data[off:off+4].hex()}', file=sys.stderr)
    sys.exit(1)
data[off:off+4] = new
open(sys.argv[2], 'wb').write(data)
" "$tmp/iBSS.raw" "$INPUTS/iBSS.n104.20E247.sigpatched.raw"

sha=$(shasum -a 256 "$INPUTS/iBSS.n104.20E247.sigpatched.raw" | awk '{print $1}')
if [ "$sha" = "$IBSS_SHA256" ]; then
    note "ok: iBSS checksum matches"
else
    fail "iBSS checksum mismatch: expected $IBSS_SHA256, got $sha"
fi

# ---------- 3. IM4M: minimal manifest (CHIP 0x8030, BORD 0x04) --------------

# The patched iBSS does not verify the signature, so the IM4M is only the
# container's shape.  A minimal DER-encoded IM4M with the right chip and
# board IDs satisfies ipsw img4 create and the iBSS's IMG4 parser.

say "generating IM4M (CHIP 0x8030, BORD 0x04)"
python3 -c "
import sys

def tl(tag, d):
    l = len(d)
    if l < 0x80:     h = bytes([tag, l])
    elif l < 0x100:  h = bytes([tag, 0x81, l])
    elif l < 0x10000: h = bytes([tag, 0x82, l >> 8, l & 0xff])
    else:            h = bytes([tag, 0x83, l >> 16, (l >> 8) & 0xff, l & 0xff])
    return h + d

def seq(d):   return tl(0x30, d)
def sett(d):  return tl(0x31, d)
def integer(v):
    b = v.to_bytes((v.bit_length() + 8) // 8, 'big') if v else b'\x00'
    return tl(0x02, b)
def ia5(s):   return tl(0x16, s.encode())
def octet(d): return tl(0x04, d)

chip = seq(ia5('CHIP') + integer(0x8030))
bord = seq(ia5('BORD') + integer(0x04))
manp = seq(ia5('MANP') + sett(chip + bord))
manb = seq(ia5('MANB') + sett(manp))
im4m = seq(ia5('IM4M') + integer(0) + sett(manb) + octet(b'\x00' * 64))

with open(sys.argv[1], 'wb') as f:
    f.write(im4m)
print(f'    wrote {len(im4m)} bytes')
" "$INPUTS/IM4M"

# ---------- 4. DeviceTree: extract and wrap as rdtr IMG4 ---------------------

say "preparing DeviceTree"
ipsw img4 im4p extract "$DT_IM4P" -o "$tmp/dt.raw"

sha=$(shasum -a 256 "$tmp/dt.raw" | awk '{print $1}')
if [ "$sha" = "$DT_SHA256" ]; then
    note "ok: DeviceTree checksum matches"
else
    fail "DeviceTree checksum mismatch: expected $DT_SHA256, got $sha"
fi

ipsw img4 create --input "$tmp/dt.raw" --type rdtr \
    --im4m "$INPUTS/IM4M" --output "$INPUTS/DeviceTree.n104.rdtr.img4"

# ---------- 5. empty trust cache ---------------------------------------------

say "creating empty trust cache"
{ printf '\001\0\0\0'; head -c 16 /dev/urandom; printf '\0\0\0\0'; } > "$tmp/empty_trustcache"
ipsw img4 create --input "$tmp/empty_trustcache" --type rtsc \
    --im4m "$INPUTS/IM4M" --output "$INPUTS/empty.rtsc.img4"

# ---------- done -------------------------------------------------------------

say "done — all four inputs are in boot/inputs/"
ls -l "$INPUTS/iBSS.n104.20E247.sigpatched.raw" \
      "$INPUTS/IM4M" \
      "$INPUTS/DeviceTree.n104.rdtr.img4" \
      "$INPUTS/empty.rtsc.img4"
echo
note "next: a13/cascadia build && a13/cascadia pack"
