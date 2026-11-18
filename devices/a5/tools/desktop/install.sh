#!/bin/sh
# tools/desktop/install.sh -- this directory's commands onto the iPad
# (DEVICE=root@10.55.0.2), after labwc/labwc-setup.sh: desktop (and
# whether at boot), rotate, autorotate and
# rotation-lock in /usr/local/bin, the rotation lock's button (put on the
# panel by autorotate at the next login) and its icons, and the autostart
# entry that runs autorotate in each XFCE session.  Running it again
# replaces them.
set -eu
D="$(cd "$(dirname "$0")" && pwd)"
DEVICE="${DEVICE:-root@10.55.0.2}"
put() {  # FILE DEST [MODE]
    ssh "$DEVICE" "cat > '$2' && chmod ${3:-644} '$2'" < "$1"
}
ssh "$DEVICE" 'mkdir -p /usr/local/bin /usr/share/applications /etc/xdg/autostart /usr/share/icons/hicolor/scalable/apps'
# BOARD=k93: the iPad 2's root, with XFCE: its `desktop` from before this
# one is desktop-xfce (xfce-setup.sh writes it so), and XFCE's autostart
# entry and panel button for the rotation
if [ "${BOARD:-p105}" = k93 ]; then
    ssh "$DEVICE" 'if [ -f /usr/local/bin/desktop ] && grep -q startxfce4 /usr/local/bin/desktop &&
                      [ ! -e /usr/local/bin/desktop-xfce ]; then
                       mv /usr/local/bin/desktop /usr/local/bin/desktop-xfce; fi'
fi
put "$D/desktop.sh" /usr/local/bin/desktop 755
put "$D/rotate.sh" /usr/local/bin/rotate 755
put "$D/autorotate.sh" /usr/local/bin/autorotate 755
put "$D/rotation-lock.sh" /usr/local/bin/rotation-lock 755
if [ "${BOARD:-p105}" = k93 ]; then
    put "$D/rotation-lock.desktop" /usr/share/applications/rotation-lock.desktop
    put "$D/cascadia-autorotate.desktop" /etc/xdg/autostart/cascadia-autorotate.desktop
fi
for f in "$D"/icons/*.svg; do
    put "$f" "/usr/share/icons/hicolor/scalable/apps/$(basename "$f")"
done
ssh "$DEVICE" 'gtk-update-icon-cache -f -q /usr/share/icons/hicolor 2>/dev/null || true'
echo "installed on $DEVICE: desktop, rotate, autorotate, rotation-lock; autorotate starts with the next session"
