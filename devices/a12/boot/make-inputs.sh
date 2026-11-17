#!/usr/bin/env bash
# Fetch the 22H374 iBSS and DeviceTree for A12_BOARD (n841, the default, or
# d331p) from Apple's CDN, without the whole IPSW. IPSW=/path/to.ipsw reads a
# local copy instead.
set -euo pipefail
A12="$(cd "$(dirname "$0")/.." && pwd)"
BOARD="${A12_BOARD:-n841}"
case "$BOARD" in
n841)
    URL='https://updates.cdn-apple.com/2026WinterFCS/fullrestores/140-97953/5F3051CD-5405-4FEF-889E-C3B78E0F376F/iPhone11,8_18.7.10_22H374_Restore.ipsw'
    DEST="$A12/boot/extracted"
    IBSS_SHA=8d748ce2190a5889ecd4933bd212f90d1308d44d8cee1880ed92b4bbeb057753
    DT_SHA=a62d565e5c7537a7aca96b5cce89f6f1111ca48a3b06c179cf0ee570bc6be269 ;;
d331p)
    URL='https://updates.cdn-apple.com/2026WinterFCS/fullrestores/140-97977/CB8B320D-008B-4FC2-8256-56A1DB86C586/iPhone11,2,iPhone11,4,iPhone11,6_18.7.10_22H374_Restore.ipsw'
    DEST="$A12/boot/extracted/d331p"
    IBSS_SHA=53f5171fa86a0953065ec16b971bd0e861497cf042e071801d55d83ef275c22c
    DT_SHA=752a1eead27fba2ed275b8866d81a4eeb4db9777dd8b903d745267eb7df8aaac
    # The GPU firmware the agx driver was written against is 16.6.1's.
    # GFX_IPSW=/path/to.ipsw reads a local copy.
    GFX_URL='https://updates.cdn-apple.com/2023SummerFCS/fullrestores/042-43832/D00DE3E3-6FAA-4DAA-B17D-94CEF1B5CA1C/iPhone11,2,iPhone11,4,iPhone11,6_16.6.1_20G81_Restore.ipsw'
    GFX_SHA=91e17c9165ac861d032ca911694fbea7840e09fb53bb9ef31fe6cee3bd1bfe7f ;;
*) echo "A12_BOARD must be n841 or d331p" >&2; exit 2 ;;
esac
IBSS="Firmware/dfu/iBSS.$BOARD.RELEASE.im4p"
DT="Firmware/all_flash/DeviceTree.${BOARD}ap.im4p"
for tool in ipsw img4 python3 sha256sum; do command -v "$tool" >/dev/null || { echo "missing $tool" >&2; exit 1; }; done
mkdir -p "$DEST"
pattern="^(BuildManifest\\.plist|${IBSS//./\\.}|${DT//./\\.})\$"
if [ -n "${IPSW:-}" ]; then
    ipsw extract --pattern "$pattern" -o "$DEST" "$IPSW"
else
    ipsw extract --remote --pattern "$pattern" -o "$DEST" "$URL"
fi
SRC="$(ls -d "$DEST"/22H374__*/ | head -1)"
python3 - "$SRC/BuildManifest.plist" "${BOARD}ap" "$IBSS" "$DT" <<'PY'
import plistlib, sys
p = plistlib.load(open(sys.argv[1], 'rb'))
assert p['ProductBuildVersion'] == '22H374'
identities = [i for i in p['BuildIdentities'] if i.get('Info', {}).get('DeviceClass') == sys.argv[2]]
assert identities
for i in identities:
    m = i['Manifest']
    assert m['iBSS']['Info']['Path'] == sys.argv[3]
    assert m['DeviceTree']['Info']['Path'] == sys.argv[4]
PY
img4 -i "$SRC/$IBSS" -o "$DEST/iBSS.raw"
img4 -i "$SRC/$DT" -o "$DEST/DeviceTree.raw"
# The GPU's firmware, loaded by iBoot. Only tried on the XS Max.
if [ -n "${GFX_SHA:-}" ]; then
    ipsw extract $([ -n "${GFX_IPSW:-}" ] || echo --remote) \
        --pattern '^Firmware/agx/armfw_g11p\.im4p$' -o "$DEST/gfx" "${GFX_IPSW:-$GFX_URL}"
    cp "$DEST"/gfx/*/Firmware/agx/armfw_g11p.im4p "$DEST/armfw_g11p.im4p"
    printf '%s  %s\n' "$GFX_SHA" "$DEST/armfw_g11p.im4p" | sha256sum -c -
fi
printf '%s  %s\n' "$IBSS_SHA" "$DEST/iBSS.raw" "$DT_SHA" "$DEST/DeviceTree.raw" | sha256sum -c -
echo "$BOARD 22H374 iBSS and DeviceTree ready in $DEST"
