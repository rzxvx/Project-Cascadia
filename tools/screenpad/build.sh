#!/bin/sh
# tools/screenpad/build.sh [install] -- screenpad (the on-screen
# controller) for the iPad: static ARM, built in the cascadia-build image
# into build/screenpad/.  "install": onto the iPad (DEVICE=root@10.55.0.2),
# /usr/local/bin/screenpad, the presets that come with it
# (/usr/local/share/screenpad/presets), screenpad-editor (GTK 3 in Python:
# py3-gobject3 and py3-cairo, added with apk if missing) and the menu's
# entries: the editor, and SuperTux with the controls.
# Needs a kernel whose apple-sgx has the screen's overlay (2026-10-23 on;
# ./cascadia build).
set -eu
D="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$D/../.." && pwd)"
DEVICE="${DEVICE:-root@10.55.0.2}"
OUT="$ROOT/build/screenpad"
mkdir -p "$OUT"
docker run --rm -v "$ROOT":/cascadia -w /cascadia cascadia-build \
    arm-linux-gnueabihf-gcc -static -O2 -Wall -Wextra -Wno-unused-parameter \
        -Wno-sign-compare -Wno-missing-field-initializers \
        -I patches/files/include/uapi/drm -I /usr/arm-linux-gnueabihf/include/drm \
        -o build/screenpad/screenpad tools/screenpad/screenpad.c -lm
ls -l "$OUT/screenpad"
if [ "${1:-}" = install ]; then
    ssh "$DEVICE" 'mkdir -p /usr/local/bin /usr/local/share/screenpad/presets'
    ssh "$DEVICE" 'cat > /usr/local/bin/screenpad.new && chmod 755 /usr/local/bin/screenpad.new &&
                   mv /usr/local/bin/screenpad.new /usr/local/bin/screenpad' < "$OUT/screenpad"
    ssh "$DEVICE" 'cat > /usr/local/bin/screenpad-editor && chmod 755 /usr/local/bin/screenpad-editor' \
        < "$D/screenpad-editor.py"
    ssh "$DEVICE" 'apk info -e py3-gobject3 py3-cairo >/dev/null ||
                       apk add --no-progress py3-gobject3 py3-cairo'
    ssh "$DEVICE" 'cat > /usr/share/applications/screenpad-editor.desktop' < "$D/screenpad-editor.desktop"
    for P in "$D"/presets/*.json; do
        ssh "$DEVICE" "cat > /usr/local/share/screenpad/presets/$(basename "$P")" < "$P"
    done
    ssh "$DEVICE" 'cat > /usr/share/applications/supertux-screenpad.desktop' < "$D/supertux-screenpad.desktop"
    echo "installed on $DEVICE: screenpad (screenpad -l: the presets), screenpad-editor"
fi
