#!/usr/bin/env bash
# Extract XR (N841) radio firmware from the matching 22H374 OS AEA/DMG.
# Usage: make-radio-firmware.sh /path/to/094-31934-038.dmg.aea
#        make-radio-firmware.sh /path/to/094-31934-038.dmg
# Apple firmware is written only to ignored boot/inputs/brcm/.
set -euo pipefail

A12="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$A12/.." && pwd)"
OUT="$A12/boot/inputs/brcm"
REV=03db9c6ae04141eb940f3b9f56d446f50d57fadf
HCDPACK_SHA=ad2f5f370501ac59cd822468c064b56a7b332dcdbbf68a67e01c1ed02e75d4ef
BLUETOOL_SHA=296cc1b898c38c0f8fa4de9f30342f6d78228c1a267360a4d4b7e5cdcd66ccd6
HCD_SHA=9cbde2943a779a4e5bfd7ab2ed6bd96598c40699609fd9f718041a6aee84e365
WIFI_DIR=System/Library/DriverExtensions/com.apple.DriverKit-AppleBCMWLAN.dext/Firmware

[[ $# == 1 && -s "$1" ]] || {
  echo "usage: $0 /path/to/094-31934-038.dmg[.aea]" >&2; exit 2;
}
for tool in 7z python3 curl cc sha256sum; do
  command -v "$tool" >/dev/null || { echo "missing $tool" >&2; exit 1; }
done

tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
image="$1"
if [[ "$image" == *.aea ]]; then
  command -v ipsw >/dev/null || { echo "missing ipsw" >&2; exit 1; }
  echo "Decrypting OS AEA into temporary space (approximately 7.1 GiB)..."
  mkdir -p "$tmp/os"
  ipsw fw aea "$image" -o "$tmp/os"
  image="$tmp/os/$(basename "${image%.aea}")"
  [[ -s "$image" ]] || { echo "AEA decryption did not produce $image" >&2; exit 1; }
fi

echo "Extracting only N841 Wi-Fi firmware and BlueTool from OS APFS..."
mkdir -p "$tmp/files"
7z x -y -bso0 -bsp0 "-o$tmp/files" "$image" "$WIFI_DIR/*" usr/sbin/BlueTool
wifi="$tmp/files/$WIFI_DIR"
bluetool="$tmp/files/usr/sbin/BlueTool"
[[ -d "$wifi" && -s "$bluetool" ]] || {
  echo "Wi-Fi firmware or BlueTool missing: use the 22H374 OS image" >&2; exit 1;
}
printf '%s  %s\n' "$BLUETOOL_SHA" "$bluetool" | sha256sum -c -

echo "Converting Broadcom Wi-Fi files for the yodak board..."
mkdir -p "$tmp/converted"
python3 "$ROOT/a13/tools/brcmfw.py" "$wifi" "$tmp/converted" yodak

echo "Building pinned Project Sandcastle hcdpack..."
curl -fsSL "https://raw.githubusercontent.com/corellium/projectsandcastle/$REV/hcdpack/hcdpack.c" \
  -o "$tmp/hcdpack.c"
printf '%s  %s\n' "$HCDPACK_SHA" "$tmp/hcdpack.c" | sha256sum -c -
cc -O2 -Wall -I"$tmp" "$tmp/hcdpack.c" -o "$tmp/hcdpack"
"$tmp/hcdpack" "$bluetool" 'C-4357__s-B1' 'Olympic Yoda-K_US' 'V-u' \
  "$tmp/converted/BCM4349B1.apple,n841.hcd"
echo
printf '%s  %s\n' "$HCD_SHA" "$tmp/converted/BCM4349B1.apple,n841.hcd" | sha256sum -c -

[[ -s "$tmp/converted/brcm/brcmfmac4357b1-pcie.apple,yodak.bin" ]] || {
  echo "no yodak Wi-Fi firmware produced" >&2; exit 1;
}
mkdir -p "$OUT"
cp "$tmp/converted/brcm/"* "$OUT/"
cp "$tmp/converted/BCM4349B1.apple,n841.hcd" "$OUT/"
echo "XR Wi-Fi and Bluetooth firmware ready in $OUT"
