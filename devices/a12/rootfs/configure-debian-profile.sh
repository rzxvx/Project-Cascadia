#!/usr/bin/env bash
set -euo pipefail
A12="$(cd "$(dirname "$0")/.." && pwd)"
source "$A12/rootfs/debian-profile.sh"
ROOT="${1:?usage: configure-debian-profile.sh ROOT}"
test -f "$ROOT/etc/passwd"
echo "$HOST_NAME" > "$ROOT/etc/hostname"
printf 'Project Cascadia — Debian ARM64 on %s\n' "$DISPLAY_NAME" > "$ROOT/etc/motd"
if ! grep -q "^127.0.1.1[[:space:]].*$HOST_NAME" "$ROOT/etc/hosts"; then
    printf '\n127.0.1.1 %s\n' "$HOST_NAME" >> "$ROOT/etc/hosts"
fi
mkdir -p "$ROOT/etc/lightdm/lightdm.conf.d"
rm -f "$ROOT/etc/lightdm/lightdm.conf.d/50-xr-autologin.conf" "$ROOT/etc/lightdm/lightdm.conf.d/50-cascadia-autologin.conf"
cat > "$ROOT/etc/lightdm/lightdm.conf.d/60-cascadia-profile.conf" <<CONF
[Seat:*]
greeter-session=lightdm-gtk-greeter
user-session=xfce
autologin-user=
autologin-guest=false
CONF
if [ "$A12_AUTOLOGIN" = 1 ]; then
    echo "autologin-user=$DESKTOP_USER" >> "$ROOT/etc/lightdm/lightdm.conf.d/60-cascadia-profile.conf"
fi
mkdir -p "$ROOT/etc/lightdm/lightdm-gtk-greeter.conf.d" "$ROOT/usr/local/bin"
install -m 755 "$A12/rootfs/debian-overlay/usr/local/bin/cascadia-greeter-keyboard" "$ROOT/usr/local/bin/cascadia-greeter-keyboard"
cat > "$ROOT/etc/lightdm/lightdm-gtk-greeter.conf.d/60-cascadia-keyboard.conf" <<'CONF'
[greeter]
indicators=~host;~spacer;~clock;~spacer;~a11y;~session;~power
keyboard=/usr/local/bin/cascadia-greeter-keyboard
keyboard-position=50%,center -0;95% 30%
a11y-states=+keyboard
CONF
if [ -f "$ROOT/etc/gdm3/daemon.conf" ]; then
    AUTO_GDM=false
    [ "$A12_AUTOLOGIN" != 1 ] || AUTO_GDM=true
    sed -i -e "s/^AutomaticLogin=.*/AutomaticLogin=$DESKTOP_USER/" -e "s/^AutomaticLoginEnable=.*/AutomaticLoginEnable=$AUTO_GDM/" "$ROOT/etc/gdm3/daemon.conf"
fi
rm -f "$ROOT/etc/AccountsService/users/xr"
# No passwords in source or command arguments. Interactive input when building.
if [ "${A12_SET_PASSWORD:-0}" = 1 ]; then
    chroot "$ROOT" passwd "$DESKTOP_USER"
fi

# Prepare from the persistent root while connected; execute only from RAM.
bash "$A12/initramfs/fetch-static-busybox.sh"
mkdir -p "$ROOT/usr/lib/cascadia" "$ROOT/usr/local/sbin" "$ROOT/etc/systemd/system/multi-user.target.wants"
install -m 755 "$A12/build/tools/busybox-static/extracted/usr/bin/busybox" "$ROOT/usr/lib/cascadia/busybox-static"
install -m 755 "$A12/initramfs/usbwatch" "$ROOT/usr/lib/cascadia/usbwatch"
install -m 755 "$A12/rootfs/debian-overlay/usr/local/sbin/cascadia-prepare-usbwatch" "$ROOT/usr/local/sbin/cascadia-prepare-usbwatch"
install -m 644 "$A12/rootfs/debian-overlay/etc/systemd/system/cascadia-usbwatch.service" "$ROOT/etc/systemd/system/cascadia-usbwatch.service"
ln -sfn ../cascadia-usbwatch.service "$ROOT/etc/systemd/system/multi-user.target.wants/cascadia-usbwatch.service"

# Native display/backlight permissions are automatic for the iPad profile only.
# Xfce/Xorg is not silently converted to a Wayland desktop.
if [ "$PROFILE" = ipad8 ]; then
    python3 "$A12/rootfs/install-ipad-display.py" --device ipad8 --root "$ROOT"
fi
