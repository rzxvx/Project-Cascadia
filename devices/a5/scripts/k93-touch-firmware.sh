#!/usr/bin/env bash
#
# K93.mtprops, the iPad 2 (iPad2,1) digitizer's firmware (Z2F13,2), out of the
# stock iPad2,1 iOS 8.4.1 / 12H321 IPSW -- the K93 counterpart of the
# P105.mtprops step in scripts/build-firmware.sh.  The firmware is Apple's and
# does not ship with the repository; tools/k93-prepare-gui-initramfs.py
# --touch-firmware takes the result.
#
# Runs inside the build container, which carries pycryptodome for
# scripts/rootfs-extract.py (docs/k93-bringup.md has the command).
#
# Pinned to 12H321 and iPad2,1: the root filesystem's name and key below are
# that build's.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IPSW="${IPSW:-$ROOT/iPad2,1_8.4.1_12H321_Restore.ipsw}"
OUT="$ROOT/build/firmware"

# Public root filesystem key for iPad2,1 / 12H321 (Keys:Donner 12H321
# (iPad2,1) in the community key database); useless without the device.
ROOTFS_DMG="058-24482-024.dmg"
ROOTFS_KEY="d33bfc10cc397ae008d2e56685acb172b74f0eccbe652ecd52e234ab12705980b6483b60"
MTPROPS=/usr/share/firmware/multitouch/K93.mtprops
# The same bytes iOS itself has at that path on a K93 unit (read there over
# ssh and compared).
KNOWN_MTPROPS_SHA256="71bf5e8266db64f83ae61d1e97d200604937f54ea72941b8ffdb54c124081190"

fail() { echo "error: $*" >&2; exit 1; }

[ -f "$IPSW" ] || fail "no IPSW at $IPSW -- pass IPSW=/path/to/iPad2,1_8.4.1_12H321_Restore.ipsw"

mkdir -p "$OUT"
python3 "$ROOT/scripts/rootfs-extract.py" "$IPSW" "$ROOTFS_DMG" "$ROOTFS_KEY" \
    "$MTPROPS" "$OUT/K93.mtprops"
echo "$KNOWN_MTPROPS_SHA256  $OUT/K93.mtprops" | sha256sum -c --quiet - \
    || fail "K93.mtprops is not 12H321's"
echo "ok: $OUT/K93.mtprops reproduces the reference byte for byte"
