#!/usr/bin/env bash
set -euo pipefail

A12="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${XR_SSH_HOST:-root@172.16.42.1}"
SSH_OPTS=(-F /dev/null -o BatchMode=yes -o Tunnel=no -o ConnectTimeout=8
          -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
HCD="$A12/boot/inputs/brcm/BCM4349B1.apple,n841.hcd"
BUNDLE="$A12/boot/inputs/bluez-root.tar.gz"
[[ -s "$HCD" ]] || { echo "Missing $HCD" >&2; exit 1; }
if ssh "${SSH_OPTS[@]}" "$HOST" 'test -f /etc/debian_version'; then
  echo "Debian root detected; using its native BlueZ tools"
else
  [[ -s "$BUNDLE" ]] || { echo "Missing $BUNDLE; run build-bluez-bundle.sh" >&2; exit 1; }
  cat "$BUNDLE" | ssh "${SSH_OPTS[@]}" "$HOST" 'tar -C / -xzf -'
fi

tar -C "$(dirname "$HCD")" -cf - "$(basename "$HCD")" | \
  ssh "${SSH_OPTS[@]}" "$HOST" 'set -e
    mkdir -p /lib/firmware/brcm
    tar -C /lib/firmware/brcm -xf -
    test -s /lib/firmware/brcm/BCM4349B1.apple,n841.hcd
    sha256sum /lib/firmware/brcm/BCM4349B1.apple,n841.hcd
    d=/sys/bus/serial/drivers/hci_uart_bcm
    test -d "$d"
    if test -L "$d/serial0-0"; then
      echo serial0-0 > "$d/unbind"
      sleep 1
    fi
    echo serial0-0 > "$d/bind"
    sleep 3
    if test -f /etc/debian_version; then
      # Debian is booted by systemd.  Never create a second system bus or
      # launch bluetoothd by hand: doing so makes pam_systemd/GDM connect to
      # the wrong bus and prevents Xorg from acquiring its virtual terminal.
      test -s /etc/machine-id || systemd-machine-id-setup
      systemctl restart bluetooth.service
      systemctl --quiet is-active bluetooth.service
      btmgmt power on
    else
      /sbin/start-bluetooth
    fi
    hciconfig -a
    bluetoothctl show
    dmesg | grep -iE "Bluetooth|hci0|BCM43|Patch" | tail -40'
