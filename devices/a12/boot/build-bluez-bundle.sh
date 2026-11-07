#!/usr/bin/env bash
set -euo pipefail

A12="$(cd "$(dirname "$0")/.." && pwd)"
source "$A12/profile.sh"
ROOT="$A12/build/devices/$A12_DEVICE/bluez-root"
OUT="$A12_INPUTS/bluez-root.tar.gz"
mkdir -p "$A12_INPUTS"
rm -rf "$ROOT"
mkdir -p "$ROOT"/{usr/bin,usr/sbin,usr/share/dbus-1/system.d,usr/share/dbus-1/system-services,etc/bluetooth,sbin}

copy_deps() {
    ldd "$1" | awk '{for(i=1;i<=NF;i++) if($i ~ /^\//) print $i}' | sort -u | while read -r lib; do
        mkdir -p "$ROOT$(dirname "$lib")"
        cp -L "$lib" "$ROOT$lib"
    done
}
for tool in /usr/bin/btmgmt /usr/bin/hciconfig /usr/bin/bluetoothctl \
            /usr/bin/dbus-daemon /usr/bin/dbus-uuidgen; do
    install -m 755 "$tool" "$ROOT$tool"
    copy_deps "$tool"
done
install -m 755 /usr/sbin/bluetoothd "$ROOT/usr/sbin/bluetoothd"
copy_deps /usr/sbin/bluetoothd
install -m 644 /usr/share/dbus-1/system.conf "$ROOT/usr/share/dbus-1/system.conf"
install -m 644 /usr/share/dbus-1/system.d/bluetooth.conf "$ROOT/usr/share/dbus-1/system.d/bluetooth.conf"
install -m 644 /usr/share/dbus-1/system-services/org.bluez.service "$ROOT/usr/share/dbus-1/system-services/org.bluez.service"
install -m 644 /etc/bluetooth/main.conf "$ROOT/etc/bluetooth/main.conf"
cat > "$ROOT/sbin/start-bluetooth" <<'EOF'
#!/bin/sh
set -e
mkdir -p /run/dbus /var/lib/bluetooth
[ -s /etc/machine-id ] || dbus-uuidgen --ensure=/etc/machine-id
[ -s /run/dbus/pid ] || dbus-daemon --system --fork
pidof bluetoothd >/dev/null 2>&1 || { bluetoothd & sleep 1; }
btmgmt --index 0 power on
btmgmt --index 0 info
EOF
chmod 755 "$ROOT/sbin/start-bluetooth"
tar -C "$ROOT" -czf "$OUT" .
echo "BlueZ RAM bundle: $OUT"
sha256sum "$OUT"
