#!/bin/sh
# tools/desktop/install.sh -- this directory's commands onto the iPad
# (DEVICE=root@10.55.0.2), after xfce-setup.sh: rotate, autorotate and
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
put "$D/rotate.sh" /usr/local/bin/rotate 755
put "$D/autorotate.sh" /usr/local/bin/autorotate 755
put "$D/rotation-lock.sh" /usr/local/bin/rotation-lock 755
put "$D/rotation-lock.desktop" /usr/share/applications/rotation-lock.desktop
put "$D/cascadia-autorotate.desktop" /etc/xdg/autostart/cascadia-autorotate.desktop
for f in "$D"/icons/*.svg; do
    put "$f" "/usr/share/icons/hicolor/scalable/apps/$(basename "$f")"
done
ssh "$DEVICE" 'gtk-update-icon-cache -f -q /usr/share/icons/hicolor 2>/dev/null || true'
echo "installed on $DEVICE: rotate, autorotate, rotation-lock; autorotate starts with the next XFCE session"
