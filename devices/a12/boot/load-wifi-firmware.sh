#!/usr/bin/env bash
set -euo pipefail

# Keep Broadcom blobs out of m1n1's gzip initramfs path: the XR boots with
# the original-size archive, then receives firmware over the proven USB ECM
# link before retrying the PCI driver probe.  All writes are to RAM.
A12="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${XR_SSH_HOST:-root@172.16.42.1}"
SSH_OPTS=(-F /dev/null -o BatchMode=yes -o Tunnel=no -o ConnectTimeout=8
          -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
source "$A12/profile.sh"
[ "$A12_DEVICE" = xr ] || { echo 'XR loader refuses a non-XR profile' >&2; exit 2; }
ssh "${SSH_OPTS[@]}" "$HOST" 'test "$(uname -s)" = Linux && tr "\000" "\n" < /proc/device-tree/compatible | grep -qx apple,n841' || exit 1
FIRMWARE="${A12_RADIO_DIR:-$A12_INPUTS/brcm}"
[[ -d "$FIRMWARE" ]] || { echo "Missing $FIRMWARE" >&2; exit 1; }
shopt -s nullglob
files=("$FIRMWARE"/brcmfmac4357b1-pcie.apple,yodak*)
((${#files[@]})) || { echo "No yodak firmware found" >&2; exit 1; }

ssh "${SSH_OPTS[@]}" "$HOST" 'mkdir -p /lib/firmware/brcm'
(cd "$FIRMWARE" && tar -cf - "${files[@]##*/}") | \
  ssh "${SSH_OPTS[@]}" "$HOST" 'tar -C /lib/firmware/brcm -xf -'
NVRAM_VARIANT="${XR_BRCM_NVRAM_VARIANT:-u}"
[[ "$NVRAM_VARIANT" == u || "$NVRAM_VARIANT" == m ]] || {
  echo "XR_BRCM_NVRAM_VARIANT must be u or m" >&2; exit 1;
}
ssh "${SSH_OPTS[@]}" "$HOST" "set -e
  cd /lib/firmware/brcm
  cp brcmfmac4357b1-pcie.apple,yodak-DRDO-$NVRAM_VARIANT.txt brcmfmac4357b1-pcie.txt
  cp brcmfmac4357b1-pcie.apple,yodak.clm_blob brcmfmac4357b1-pcie.clm_blob
  d=/sys/bus/pci/drivers/brcmfmac
  test -d \"\$d\"
  if test -L \"\$d/0000:01:00.0\"; then
    echo 0000:01:00.0 > \"\$d/unbind\"
  fi
  echo 0000:01:00.0 > \"\$d/bind\"
  sleep 4
  dmesg | grep -iE 'brcmf|wlan' | tail -25
  ls /sys/class/net"
