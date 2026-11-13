#!/usr/bin/env bash
set -euo pipefail

A12="$(cd "$(dirname "$0")/.." && pwd)"
source "$A12/rootfs/debian-profile.sh"
SUITE="${DEBIAN_SUITE:-trixie}"
DESKTOP="${A12_DESKTOP:-${XR_DESKTOP:-xfce}}"
case "$DESKTOP" in
    xfce) DISPLAY_MANAGER=lightdm
          DESKTOP_PACKAGES="lightdm lightdm-gtk-greeter xfce4 xfce4-terminal thunar xfce4-settings xfce4-power-manager xfce4-notifyd xfce4-whiskermenu-plugin onboard" ;;
    gnome) DISPLAY_MANAGER=gdm3
           DESKTOP_PACKAGES="gdm3 gnome-shell gnome-session gnome-session-xsession gnome-control-center gnome-terminal nautilus gnome-backgrounds gnome-calculator" ;;
    *) echo "A12_DESKTOP must be xfce or gnome" >&2; exit 2 ;;
esac
MIRROR="${DEBIAN_MIRROR:-http://deb.debian.org/debian}"
ROOT="$A12/build/debian-$PROFILE-root"
OUT="$A12/build/debian-$PROFILE.tar.gz"
PACKAGES="ca-certificates dbus dbus-user-session udev dropbear openssh-sftp-server sudo locales polkitd lxpolkit network-manager network-manager-gnome wireless-regdb iw wpasupplicant bluez bluez-tools firmware-brcm80211 xserver-xorg-core xserver-xorg-input-libinput xserver-xorg-video-fbdev xinit xinput upower power-profiles-daemon iio-sensor-proxy gsettings-desktop-schemas adwaita-icon-theme fonts-cantarell mesa-utils libgl1-mesa-dri librsvg2-common libgtk-3-bin shared-mime-info fonts-dejavu-core procps psmisc iproute2 iputils-ping kmod $DESKTOP_PACKAGES"

# Native A12 RTC time is not yet reliable in this port. Keep signed APT/TLS
# verification enabled and synchronize the clock once networking is online.
PACKAGES="$PACKAGES systemd-timesyncd"
if [ "$A12_DEVICE" = ipad8 ]; then
    PACKAGES="$PACKAGES pipewire pipewire-pulse wireplumber alsa-utils"
    [ "$DESKTOP" != xfce ] || PACKAGES="$PACKAGES xfce4-pulseaudio-plugin pavucontrol"
fi

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
    cat > "$ROOT/etc/lightdm/lightdm.conf.d/50-cascadia-autologin.conf" <<EOF
[Seat:*]
autologin-user=$DESKTOP_USER
autologin-user-timeout=0
user-session=xfce
EOF
    install -d "$ROOT/var/lib/AccountsService/users"
    cat > "$ROOT/var/lib/AccountsService/users/$DESKTOP_USER" <<'EOF'
[User]
SystemAccount=false
Session=xfce
XSession=xfce
EOF
else
    rm -rf "$ROOT/etc/lightdm/lightdm.conf.d/50-cascadia-autologin.conf"
fi
bash "$A12/rootfs/install-touch-firmware.sh" "$ROOT"
python3 "$A12/rootfs/install-ipad-audio.py" --root "$ROOT" --device "$A12_DEVICE" --desktop none
mkdir -p "$ROOT/root/.ssh" "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" "$ROOT/run"
chmod 700 "$ROOT/root" "$ROOT/root/.ssh"

# The desktop runs as an unprivileged autologin user. Xorg is used because
# the XR currently exposes a simple framebuffer rather than a DRM/KMS device.
chroot "$ROOT" useradd -m -c "$DISPLAY_NAME" -s /bin/bash "$DESKTOP_USER"
for group in video input render audio netdev bluetooth; do
    chroot "$ROOT" getent group "$group" >/dev/null 2>&1 &&
        chroot "$ROOT" usermod -aG "$group" "$DESKTOP_USER" || true
done
if [ "$A12_AUTOLOGIN" = 1 ]; then
    chroot "$ROOT" passwd -d "$DESKTOP_USER" >/dev/null
else
    # Prompt; never ship a shared default password.
    chroot "$ROOT" passwd "$DESKTOP_USER"
fi
mkdir -p "$ROOT/home/$DESKTOP_USER/.config"
touch "$ROOT/home/$DESKTOP_USER/.config/gnome-initial-setup-done"
chroot "$ROOT" chown -R "$DESKTOP_USER:$DESKTOP_USER" "/home/$DESKTOP_USER"
bash "$A12/rootfs/configure-debian-profile.sh" "$ROOT"
systemctl --root="$ROOT" enable "$DISPLAY_MANAGER.service" NetworkManager.service \
    bluetooth.service dropbear.service systemd-timesyncd.service >/dev/null
systemctl --root="$ROOT" disable NetworkManager-wait-online.service \
    >/dev/null 2>&1 || true
ln -sfn /usr/lib/systemd/system/graphical.target \
    "$ROOT/etc/systemd/system/default.target"

cleanup_mounts
trap - EXIT

# GPU Mesa installation is explicit: a host build may not match this Debian ABI.
# Set A12_INSTALL_GPU_MESA=1 and prepare the shared stage with build mesa first.
if [ "${A12_INSTALL_GPU_MESA:-0}" = 1 ]; then
    [ "$PROFILE" != xr ] || { echo 'XR GPU is not validated' >&2; exit 2; }
    python3 "$A12/mesa/runtime.py" install "${A12_MESA_STAGE:-$A12/build/mesa-stage}" --root "$ROOT"
fi

tar -C "$ROOT" -czf "$OUT" .
sha256sum "$OUT"
du -sh "$ROOT" "$OUT"
