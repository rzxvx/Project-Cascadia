#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
HOST="${XR_SSH_HOST:-root@172.16.42.1}"
SSH_OPTS=(-F /dev/null -o BatchMode=yes -o Tunnel=no -o ConnectTimeout=2
          -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)

# Fail before touching the phone if the external radio payload is incomplete.
# These blobs deliberately stay outside rkrn and are copied into Debian RAM
# only after the NFS switch_root has completed.
FIRMWARE="$HERE/inputs/brcm"
[[ -s "$FIRMWARE/brcmfmac4357b1-pcie.apple,yodak.bin" ]] || {
  echo "Missing XR Wi-Fi firmware in $FIRMWARE" >&2; exit 1;
}
[[ -s "$FIRMWARE/brcmfmac4357b1-pcie.apple,yodak-DRDO-u.txt" ]] || {
  echo "Missing XR Wi-Fi NVRAM in $FIRMWARE" >&2; exit 1;
}
[[ -s "$FIRMWARE/brcmfmac4357b1-pcie.apple,yodak.clm_blob" ]] || {
  echo "Missing XR Wi-Fi CLM blob in $FIRMWARE" >&2; exit 1;
}
[[ -s "$FIRMWARE/BCM4349B1.apple,n841.hcd" ]] || {
  echo "Missing XR Bluetooth HCD in $FIRMWARE" >&2; exit 1;
}

"$HERE/boot-xr-linux.sh"
echo "Waiting for XR USB SSH..."
for _ in $(seq 1 60); do
  # Do not race the temporary initramfs Dropbear. Wait until switch_root has
  # completed and Debian has restarted SSH before copying firmware.
  if ssh "${SSH_OPTS[@]}" "$HOST" 'test -f /etc/debian_version' >/dev/null 2>&1; then
    echo "XR Debian NFS root is online."
    "$HERE/load-wifi-firmware.sh"
    "$HERE/load-bluetooth-firmware.sh"
    echo "XR Linux, Wi-Fi, and Bluetooth are ready."
    exit 0
  fi
  sleep 1
done
echo "Timed out waiting for XR Debian NFS root; DCSD logging is still active." >&2
exit 1
