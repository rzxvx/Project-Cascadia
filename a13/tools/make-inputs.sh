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
# Needs: ipsw (3.1.714+ tested), python3, and an IM4M from an iPhone12,1
# SHSH blob (IM4M=path, or already in boot/inputs/IM4M).
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

have ipsw    || fail "ipsw not found (https://github.com/blacktop/ipsw)"
have python3 || fail "python3 not found"

# The one IPSW this port is pinned to.  Apple still serves it; the URL is
# Apple's own CDN, nothing here redistributes the firmware.
IPSW_URL="https://updates.cdn-apple.com/2023SpringFCS/fullrestores/032-67885/8B726A7C-722A-4D2D-8BEF-5A702FFEC8D5/iPhone12,1_16.4_20E247_Restore.ipsw"

# iBSS AES key (IV + Key), from pwnerblu/surrealra1n keys/iPhone12,1.txt
IBSS_IV=98df294debc43c1488cb4c563f5d3b1f
IBSS_KEY=6aaddc1c0a47f9e149ca175cc7eb02e7af4225489093aee6770f61f136f587ef

# Expected checksums (from boot/inputs/README.md)
IBSS_SHA256=5b77df1fceb136c42ced74003f6d8b5b1acacb56bda5f88d112895e4853034f2
DT_SHA256=62b7e44eb0b3f220124283d012cacb7cfdef259444c6cf8e0539ad255dafe9e2
MTFW_SHA256=de5dd11f00abae27b07f1a637686fcd03df748d1d4712a01592ac4bea4015036

# shasum on macOS, sha256sum on Linux
if have shasum; then
    sha256() { shasum -a 256 "$1" | awk '{print $1}'; }
elif have sha256sum; then
    sha256() { sha256sum "$1" | awk '{print $1}'; }
else
    fail "no shasum or sha256sum"
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# ---------- 1. download from Apple's CDN ------------------------------------
# ipsw-partial.py extracts individual files from the remote IPSW (ZIP) using
# HTTP range requests: ~1.5 MB downloaded instead of 6.4 GB.  This avoids
# `ipsw download`, whose iTunes API lookup fails on Linux.

say "downloading iBSS and DeviceTree from Apple"
python3 -I "$A13/tools/ipsw-partial.py" "$IPSW_URL" \
    'Firmware/dfu/iBSS.n104.RELEASE.im4p' \
    'Firmware/all_flash/DeviceTree.n104ap.im4p' \
    'Firmware/N104_Multitouch.im4p' \
    -o "$tmp"

IBSS_IM4P="$tmp/iBSS.n104.RELEASE.im4p"
DT_IM4P="$tmp/DeviceTree.n104ap.im4p"
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

sha=$(sha256 "$INPUTS/iBSS.n104.20E247.sigpatched.raw")
if [ "$sha" = "$IBSS_SHA256" ]; then
    note "ok: iBSS checksum matches"
else
    fail "iBSS checksum mismatch: expected $IBSS_SHA256, got $sha"
fi

# ---------- 3. IM4M: a real iPhone12,1 manifest ------------------------------

# The patched iBSS skips the signature check, but iBoot still parses the
# manifest and looks up each image's tag in it: an IM4M without rdtr / rtsc /
# rkrn entries (or not a real Apple manifest at all) gets "Device Tree image
# not valid".  So it cannot be generated -- it has to come from a saved SHSH
# blob of any iPhone12,1 (`ipsw img4 im4m extract`).  Given as IM4M=path, or
# already in boot/inputs/IM4M.

im4m="${IM4M:-$INPUTS/IM4M}"
[ -f "$im4m" ] || fail "no IM4M -- give one from an iPhone12,1 SHSH blob: IM4M=/path/to/IM4M a13/cascadia inputs  (see boot/inputs/README.md)"

say "checking IM4M ($im4m)"
info=$(ipsw img4 im4m info "$im4m" 2>&1) || fail "$im4m is not an IM4M"
printf '%s\n' "$info" | grep -q 'CHIP (ApChipID): 32816$' || fail "$im4m is not for CHIP 0x8030"
printf '%s\n' "$info" | grep -q 'BORD (ApBoardID): 4$'     || fail "$im4m is not for BORD 0x04 (iPhone12,1)"
printf '%s\n' "$info" | grep -q 'Certificate Chain: none' && fail "$im4m has no certificate chain -- not a real manifest"
for t in rdtr rtsc rkrn; do
    printf '%s\n' "$info" | grep -q "^ *$t (" || fail "$im4m has no $t entry"
done
note "ok: CHIP 0x8030, BORD 0x04, rdtr/rtsc/rkrn present"
[ "$im4m" = "$INPUTS/IM4M" ] || cp "$im4m" "$INPUTS/IM4M"

# ---------- 4. DeviceTree: extract and wrap as rdtr IMG4 ---------------------

say "preparing DeviceTree"
ipsw img4 im4p extract "$DT_IM4P" -o "$tmp/dt.raw"

sha=$(sha256 "$tmp/dt.raw")
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

# ---------- 6. touchscreen firmware -------------------------------------------

# N104_Multitouch.im4p (tag mtfw, not encrypted) is an IOKit plist with the
# controller's firmware and boot recipe for each panel personality; the ADT's
# hid-merge-personality is C1FC2,2. mtfw.py turns it into the "Z2FW" file the
# kernel driver loads, and saves the plist's default calibration (used when
# the boot chain gives no MtCl).

say "preparing touchscreen firmware"
[ -f "$tmp/N104_Multitouch.im4p" ] || fail "N104_Multitouch.im4p download failed"
ipsw img4 im4p extract "$tmp/N104_Multitouch.im4p" -o "$tmp/mt.raw" >/dev/null
python3 -I "$A13/tools/mtfw.py" "$tmp/mt.raw" C1FC2,2 \
    "$INPUTS/n104-mtfw.bin" "$INPUTS/n104-mtcal.bin" >/dev/null
sha=$(sha256 "$INPUTS/n104-mtfw.bin")
[ "$sha" = "$MTFW_SHA256" ] || fail "n104-mtfw.bin checksum mismatch: expected $MTFW_SHA256, got $sha"
note "ok: n104-mtfw.bin checksum matches, n104-mtcal.bin"

# ---------- done -------------------------------------------------------------

say "done — the inputs are in boot/inputs/"
ls -l "$INPUTS/iBSS.n104.20E247.sigpatched.raw" \
      "$INPUTS/IM4M" \
      "$INPUTS/DeviceTree.n104.rdtr.img4" \
      "$INPUTS/empty.rtsc.img4" \
      "$INPUTS/n104-mtfw.bin" "$INPUTS/n104-mtcal.bin"
echo
note "next: a13/cascadia build && a13/cascadia pack"
