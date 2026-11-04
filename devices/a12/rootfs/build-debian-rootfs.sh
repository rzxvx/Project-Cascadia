#!/usr/bin/env bash
set -euo pipefail

A12="$(cd "$(dirname "$0")/.." && pwd)"
SUITE="${DEBIAN_SUITE:-trixie}"
DESKTOP="${XR_DESKTOP:-xfce}"
case "$DESKTOP" in
    xfce) DISPLAY_MANAGER=lightdm
          DESKTOP_PACKAGES="lightdm xfce4 xfce4-terminal thunar xfce4-settings xfce4-power-manager xfce4-notifyd xfce4-whiskermenu-plugin onboard" ;;
    gnome) DISPLAY_MANAGER=gdm3
           DESKTOP_PACKAGES="gdm3 gnome-shell gnome-session gnome-session-xsession gnome-control-center gnome-terminal nautilus gnome-backgrounds gnome-calculator" ;;
    *) echo "XR_DESKTOP must be xfce or gnome" >&2; exit 2 ;;
esac
MIRROR="${DEBIAN_MIRROR:-http://deb.debian.org/debian}"
ROOT="$A12/build/debian-xr-root"
OUT="$A12/build/debian-xr.tar.gz"
PACKAGES="ca-certificates dbus dbus-user-session udev dropbear openssh-sftp-server sudo locales network-manager network-manager-gnome wireless-regdb iw wpasupplicant bluez bluez-tools firmware-brcm80211 xserver-xorg-core xserver-xorg-input-libinput xserver-xorg-video-fbdev xinit xinput upower power-profiles-daemon iio-sensor-proxy gsettings-desktop-schemas adwaita-icon-theme fonts-cantarell mesa-utils libgl1-mesa-dri librsvg2-common libgtk-3-bin shared-mime-info fonts-dejavu-core procps psmisc iproute2 iputils-ping kmod $DESKTOP_PACKAGES"

command -v debootstrap >/dev/null || {
    echo "install debootstrap first" >&2
    exit 1
}

rm -rf "$ROOT"
mkdir -p "$ROOT"
debootstrap --arch=arm64 --variant=minbase "$SUITE" "$ROOT" "$MIRROR"

# Package maintainer scripts need working device nodes, procfs and sysfs.
# The project filesystem can store device nodes but cannot necessarily open
# them, so use the host virtual filesystems while operating in the chroot.
cleanup_mounts() {
    umount "$ROOT/dev/pts" 2>/dev/null || true
    umount "$ROOT/dev" 2>/dev/null || true
    umount "$ROOT/proc" 2>/dev/null || true
    umount "$ROOT/sys" 2>/dev/null || true
}
trap cleanup_mounts EXIT
mount --bind /dev "$ROOT/dev"
mount --bind /dev/pts "$ROOT/dev/pts"
mount -t proc proc "$ROOT/proc"
mount --bind /sys "$ROOT/sys"

cat >"$ROOT/etc/apt/sources.list" <<EOF
deb $MIRROR $SUITE main contrib non-free-firmware
deb $MIRROR ${SUITE}-updates main contrib non-free-firmware
deb http://security.debian.org/debian-security ${SUITE}-security main contrib non-free-firmware
EOF
rm -f "$ROOT/etc/resolv.conf"
cp /etc/resolv.conf "$ROOT/etc/resolv.conf"
chroot "$ROOT" apt-get update
chroot "$ROOT" env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends $PACKAGES
chroot "$ROOT" update-mime-database /usr/share/mime
chroot "$ROOT" sh -c '/usr/lib/aarch64-linux-gnu/gdk-pixbuf-2.0/gdk-pixbuf-query-loaders > /usr/lib/aarch64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders.cache'
chroot "$ROOT" apt-get clean

# Install board-specific Xorg, display manager and NetworkManager configuration. Do not
# overlay /sbin/init: Debian's merged-/usr /sbin points at /usr/sbin and doing
# so would overwrite the real systemd binary required by GNOME/logind.
cp -a "$A12/rootfs/debian-overlay/etc/." "$ROOT/etc/"
if [ "$DESKTOP" = xfce ]; then
    rm -f "$ROOT/etc/gdm3/daemon.conf"
    rm -f "$ROOT/etc/AccountsService/users/xr"
    install -d "$ROOT/etc/lightdm/lightdm.conf.d"
    cat > "$ROOT/etc/lightdm/lightdm.conf.d/50-xr-autologin.conf" <<'EOF'
[Seat:*]
autologin-user=xr
autologin-user-timeout=0
user-session=xfce
EOF
    install -d "$ROOT/var/lib/AccountsService/users"
    cat > "$ROOT/var/lib/AccountsService/users/xr" <<'EOF'
[User]
SystemAccount=false
Session=xfce
XSession=xfce
EOF
else
    rm -rf "$ROOT/etc/lightdm/lightdm.conf.d/50-xr-autologin.conf"
fi
mkdir -p "$ROOT/lib/firmware/apple"
for name in n841-mtfw.bin n841-mtcal.bin; do
    test -s "$A12/build/touch/$name" || {
        echo "missing XR touch firmware: $A12/build/touch/$name" >&2
        exit 1
    }
    install -m 644 "$A12/build/touch/$name" "$ROOT/lib/firmware/apple/$name"
done
mkdir -p "$ROOT/root/.ssh" "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" "$ROOT/run"
chmod 700 "$ROOT/root" "$ROOT/root/.ssh"

# The desktop runs as an unprivileged autologin user. Xorg is used because
# the XR currently exposes a simple framebuffer rather than a DRM/KMS device.
chroot "$ROOT" useradd -m -s /bin/bash xr
for group in video input render audio netdev bluetooth; do
    chroot "$ROOT" getent group "$group" >/dev/null 2>&1 &&
        chroot "$ROOT" usermod -aG "$group" xr || true
done
chroot "$ROOT" passwd -d xr >/dev/null
mkdir -p "$ROOT/home/xr/.config"
touch "$ROOT/home/xr/.config/gnome-initial-setup-done"
chroot "$ROOT" chown -R xr:xr /home/xr
systemctl --root="$ROOT" enable "$DISPLAY_MANAGER.service" NetworkManager.service \
    bluetooth.service dropbear.service >/dev/null
systemctl --root="$ROOT" disable NetworkManager-wait-online.service \
    >/dev/null 2>&1 || true
ln -sfn /usr/lib/systemd/system/graphical.target \
    "$ROOT/etc/systemd/system/default.target"

cleanup_mounts
trap - EXIT

tar -C "$ROOT" -czf "$OUT" .
sha256sum "$OUT"
du -sh "$ROOT" "$OUT"
