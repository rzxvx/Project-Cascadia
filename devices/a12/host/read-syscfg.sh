#!/usr/bin/env bash
# Read only the selected device's factory SysCfg; never publish it.
set -euo pipefail
A12="$(cd "$(dirname "$0")/.." && pwd)"
source "$A12/profile.sh"
OUT="${1:-$A12_SYSCFG}"
HOST="${A12_SSH_HOST:-${XR_SSH_HOST:-root@172.16.42.1}}"
SSH_OPTS=(-F /dev/null -o BatchMode=yes -o Tunnel=no -o ConnectTimeout=8
          -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
[ ! -e "$OUT" ] || { echo "refusing to overwrite $OUT" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"
umask 077
tmp=$(mktemp "${OUT}.tmp.XXXXXX")
trap 'rm -f -- "$tmp"' EXIT
ssh "${SSH_OPTS[@]}" "$HOST" "test \"\$(uname -s)\" = Linux && tr '\\000' '\\n' < /proc/device-tree/compatible | grep -qx apple,$A12_BOARD" || exit 1
ssh "${SSH_OPTS[@]}" "$HOST" '
  set -e
  test -b /dev/nvme0n3
  test "$(blockdev --getsize64 /dev/nvme0n3)" = 131072
  blockdev --setro /dev/nvme0n3
  dd if=/dev/nvme0n3 bs=4096 count=32 2>/dev/null
' > "$tmp"
python3 "$A12/profile.py" --check-syscfg "$tmp"
chmod 600 "$tmp"
# Exclusive link creation avoids overwriting a concurrently created file.
ln "$tmp" "$OUT"
echo "Saved private $A12_LABEL SysCfg (0600); values withheld"
