#!/usr/bin/env bash
# External RAM-only radio firmware loading for J171a. Never used by XR.
set -euo pipefail
A12="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${IPAD8_SSH_HOST:-root@172.16.42.1}"
export A12_DEVICE="${A12_DEVICE:-ipad8}"
source "$A12/profile.sh"
[ "$A12_DEVICE" = ipad8 ] || { echo "iPad loader refuses a non-iPad profile" >&2; exit 2; }
FW="${IPAD8_RADIO_DIR:-$A12_INPUTS/radios}"
SSH_OPTS=(-F /dev/null -o BatchMode=yes -o Tunnel=no -o ConnectTimeout=8
          -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
usage() {
  echo "usage: $0 probe | wifi [m|u] | bluetooth [m|u] | diagnose" >&2
  echo "Firmware loading requires the module vendor verified from OTP: m=Murata, u=USI." >&2
}
[[ $# -ge 1 ]] || { usage; exit 2; }
mode="$1"
case "$mode" in
  probe|diagnose) [[ $# == 1 ]] || { usage; exit 2; } ;;
  wifi|bluetooth) [[ $# == 2 && ( "$2" == m || "$2" == u ) ]] || { usage; exit 2; } ;;
  *) usage; exit 2 ;;
esac
# Check before transferring files, changing power or rebinding any driver.
ssh "${SSH_OPTS[@]}" "$HOST" '
  test "$(uname -s)" = Linux &&
  tr "\000" "\n" < /proc/device-tree/compatible | grep -qx apple,j171a
' || { echo "Refusing: target is not J171a Linux" >&2; exit 1; }

case "$mode" in
  probe)
    ssh "${SSH_OPTS[@]}" "$HOST" '
      set -e
      # Only enable the OTP summary, not an unbounded raw register dump.
      control=/sys/kernel/debug/dynamic_debug/control
      if test -w "$control"; then
        echo "func brcmf_pcie_parse_otp_sys_vendor +p" > "$control"
      fi
      count=0; id=
      for p in /sys/bus/pci/devices/*; do
        test -f "$p/vendor" && test -f "$p/device" || continue
        if test "$(cat "$p/vendor")" = 0x14e4 && test "$(cat "$p/device")" = 0x43dc; then
          id=${p##*/}; count=$((count+1))
        fi
      done
      test "$count" = 1 || { echo "BCM4355 not uniquely enumerated; inspect PCIe log" >&2; exit 1; }
      d=/sys/bus/pci/drivers/brcmfmac
      test -d "$d"
      if test -L "$d/$id"; then echo "$id" > "$d/unbind"; fi
      # Missing firmware is expected: this step reads OTP, not a guessed HCD.
      if ! echo "$id" > "$d/bind"; then echo "Probe did not bind; inspect diagnostics below" >&2; fi
      sleep 2
      dmesg | grep -iE "OTP:|brcmf|pcie" | tail -50
    '
    ;;
  wifi)
    files=(brcmfmac4355c1-pcie.apple,magnoliaa.bin
           brcmfmac4355c1-pcie.apple,magnoliaa-YSBC-m.txt
           brcmfmac4355c1-pcie.apple,magnoliaa-YSBC-u.txt
           brcmfmac4355c1-pcie.apple,magnoliaa.clm_blob
           brcmfmac4355c1-pcie.apple,magnoliaa.txcap_blob)
    for f in "${files[@]}"; do
      [[ -s "$FW/brcm/$f" ]] || { echo "Missing $FW/brcm/$f" >&2; exit 1; }
    done
    (cd "$FW/brcm" && tar -cf - "${files[@]}") | ssh "${SSH_OPTS[@]}" "$HOST" '
      set -e
      mkdir -p /lib/firmware/brcm
      tar -C /lib/firmware/brcm -xf -
    '
    # SysCfg on this iPad lacks WSKU, so automatic Apple selection is not
    # entered by brcmfmac. Supply only the explicitly verified vendor NVRAM.
    ssh "${SSH_OPTS[@]}" "$HOST" "cp /lib/firmware/brcm/brcmfmac4355c1-pcie.apple,magnoliaa-YSBC-$2.txt /lib/firmware/brcm/brcmfmac4355c1-pcie.apple,magnoliaa.txt"
    ssh "${SSH_OPTS[@]}" "$HOST" '
      set -e
      count=0; id=
      for p in /sys/bus/pci/devices/*; do
        test -f "$p/vendor" && test -f "$p/device" || continue
        if test "$(cat "$p/vendor")" = 0x14e4 && test "$(cat "$p/device")" = 0x43dc; then
          id=${p##*/}; count=$((count+1))
        fi
      done
      test "$count" = 1 || { echo "Expected exactly one BCM4355 PCI device; found $count" >&2; exit 1; }
      d=/sys/bus/pci/drivers/brcmfmac
      test -d "$d"
      if test -L "$d/$id"; then echo "$id" > "$d/unbind"; fi
      echo "$id" > "$d/bind"
      sleep 4
      dmesg | grep -iE "brcmf|wlan|OTP|firmware" | tail -50
      ls /sys/class/net
    '
    ;;
  bluetooth)
    vendor=MUR; [[ "$2" == u ]] && vendor=USI
    hcd="$FW/bluetooth-candidates/BCM4355C1.MagnoliaA.$vendor.hcd"
    [[ -s "$hcd" ]] || { echo "Missing $hcd" >&2; exit 1; }
    # btbcm's board-specific fallback avoids assuming its HCI chip-name alias.
    ssh "${SSH_OPTS[@]}" "$HOST" 'mkdir -p /lib/firmware/brcm'
    cat "$hcd" | ssh "${SSH_OPTS[@]}" "$HOST" '
      set -e
      target=/lib/firmware/brcm/BCM.apple,j171a.hcd
      cat > "$target"
      test -s "$target"
      sha256sum "$target"
      count=0; id=
      for p in /sys/bus/serial/devices/*; do
        test -r "$p/of_node/compatible" || continue
        if tr "\000" "\n" < "$p/of_node/compatible" | grep -qx brcm,bcm4329-bt; then
          id=${p##*/}; count=$((count+1))
        fi
      done
      test "$count" = 1 || { echo "Expected one Broadcom UART child; found $count" >&2; exit 1; }
      d=/sys/bus/serial/drivers/hci_uart_bcm
      test -d "$d"
      if test -L "$d/$id"; then echo "$id" > "$d/unbind"; sleep 1; fi
      echo "$id" > "$d/bind"
      sleep 3
      dmesg | grep -iE "Bluetooth|hci[0-9]|BCM|Patch" | tail -50
      ls /sys/class/bluetooth
    '
    ;;
  diagnose)
    ssh "${SSH_OPTS[@]}" "$HOST" '
      echo "=== PCI identifiers ==="
      for p in /sys/bus/pci/devices/*; do
        test -f "$p/vendor" || continue
        echo "${p##*/} $(cat "$p/vendor") $(cat "$p/device")"
      done
      echo "=== UART children ==="; ls /sys/bus/serial/devices
      echo "=== Network / Bluetooth ==="; ls /sys/class/net /sys/class/bluetooth
      echo "=== Recent radio diagnostics ==="
      dmesg | grep -iE "brcmf|Bluetooth|hci[0-9]|BCM|OTP|pcie|firmware" | tail -80
    '
    ;;
esac
