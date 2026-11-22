#!/usr/bin/env bash
# fbkeyboard.sh - the on-screen keyboard for the initramfs, as the A13 track
# has it (devices/a13/tools/make-periphery.sh): Alpine's fbkeyboard draws a
# keyboard at the bottom of the framebuffer and types into the console
# through uinput. A small self-contained tree: the binary, the musl loader,
# its libraries, DejaVu Sans. /init starts it on the touchscreen.
# Output: devices/a10/build/fbkeyboard/; pack adds it to the initramfs.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
A10="$(cd "$HERE/.." && pwd)"
OUT="$A10/build/fbkeyboard"

if [ -x "$OUT/usr/bin/fbkeyboard" ]; then
    echo "fbkeyboard OK (already there): $OUT"
    exit 0
fi
rm -rf "$OUT"; mkdir -p "$OUT"
docker run --rm --platform linux/arm64 -e HOST_IDS="$(id -u):$(id -g)" \
    -v "$OUT":/out alpine:3.24 sh -ec '
    apk add -q fbkeyboard font-dejavu >/dev/null
    for f in /usr/bin/fbkeyboard /lib/ld-musl-aarch64.so.1 \
             $(ldd /usr/bin/fbkeyboard | sed -n "s/.*=> \([^ ]*\).*/\1/p") \
             /usr/share/fonts/dejavu/DejaVuSans.ttf; do
        mkdir -p "/out$(dirname "$f")"
        cp -L "$f" "/out$f"
    done
    chown -R "$HOST_IDS" /out'
echo "fbkeyboard OK: $OUT"
