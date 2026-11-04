#!/usr/bin/env bash
# Rebuild XR 22H374 first-stage inputs from Apple's CDN, without a full IPSW.
set -euo pipefail
A12="$(cd "$(dirname "$0")/.." && pwd)"
URL='https://updates.cdn-apple.com/2026WinterFCS/fullrestores/140-97953/5F3051CD-5405-4FEF-889E-C3B78E0F376F/iPhone11,8_18.7.10_22H374_Restore.ipsw'
DEST="$A12/boot/extracted"
IBSS_SHA=8d748ce2190a5889ecd4933bd212f90d1308d44d8cee1880ed92b4bbeb057753
DT_SHA=a62d565e5c7537a7aca96b5cce89f6f1111ca48a3b06c179cf0ee570bc6be269
for tool in ipsw img4 python3 sha256sum; do command -v "$tool" >/dev/null || { echo "missing $tool" >&2; exit 1; }; done
mkdir -p "$DEST"
ipsw extract --remote --pattern '^(BuildManifest\.plist|Firmware/dfu/iBSS\.n841\.RELEASE\.im4p|Firmware/all_flash/DeviceTree\.n841ap\.im4p)$' -o "$DEST" "$URL"
SRC="$DEST/22H374__iPhone11,8"
python3 - "$SRC/BuildManifest.plist" <<'PY'
import plistlib, sys
p = plistlib.load(open(sys.argv[1], 'rb'))
assert p['ProductBuildVersion'] == '22H374'
identities = [i for i in p['BuildIdentities'] if i.get('Info', {}).get('DeviceClass') == 'n841ap']
assert identities
for i in identities:
    m = i['Manifest']
    assert m['iBSS']['Info']['Path'] == 'Firmware/dfu/iBSS.n841.RELEASE.im4p'
    assert m['DeviceTree']['Info']['Path'] == 'Firmware/all_flash/DeviceTree.n841ap.im4p'
PY
img4 -i "$SRC/Firmware/dfu/iBSS.n841.RELEASE.im4p" -o "$DEST/iBSS.raw"
img4 -i "$SRC/Firmware/all_flash/DeviceTree.n841ap.im4p" -o "$DEST/DeviceTree.raw"
printf '%s  %s\n' "$IBSS_SHA" "$DEST/iBSS.raw" "$DT_SHA" "$DEST/DeviceTree.raw" | sha256sum -c -
echo "XR 22H374 iBSS and pristine DeviceTree ready in $DEST"
echo "Next: python3 devices/a12/boot/prepare-xr-linux-bootset.py --ticket /path/to/your/apticket.im4m [--serial|--no-serial]"
