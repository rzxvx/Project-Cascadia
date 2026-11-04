#!/usr/bin/env bash
# Read this XR's factory SysCfg from Linux NVMe namespace 3 over USB SSH.
# Output stays local, private and ignored by Git. Never publish it.
set -euo pipefail

A12="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$A12/.." && pwd)"
OUT="${1:-$A12/boot/inputs/syscfg.bin}"
HOST="${XR_SSH_HOST:-root@172.16.42.1}"
SSH_OPTS=(-F /dev/null -o BatchMode=yes -o Tunnel=no -o ConnectTimeout=8
          -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)

[[ ! -e "$OUT" ]] || { echo "refusing to overwrite existing $OUT" >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"
umask 077
tmp="$(mktemp "${OUT}.tmp.XXXXXX")"
trap 'rm -f -- "$tmp"' EXIT

ssh "${SSH_OPTS[@]}" "$HOST" '
  set -e
  test "$(uname -s)" = Linux
  test -b /dev/nvme0n3
  test "$(blockdev --getsize64 /dev/nvme0n3)" = 131072
  blockdev --setro /dev/nvme0n3
  dd if=/dev/nvme0n3 bs=4096 count=32 2>/dev/null
' > "$tmp"

python3 - "$ROOT/a13/tools/syscfg.py" "$tmp" <<'PY'
import importlib.util
import pathlib
import sys

path = pathlib.Path(sys.argv[2])
data = path.read_bytes()
if len(data) != 131072 or data[:4] != b'gfCS':
    raise SystemExit('invalid XR SysCfg size or magic')
spec = importlib.util.spec_from_file_location('syscfg', sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
keys = module.parse(data)
required = {'WMac': 16, 'WSKU': 16, 'WCAL': 1, 'BMac': 16}
for name, minimum in required.items():
    if len(keys.get(name, b'')) < minimum:
        raise SystemExit(f'SysCfg missing or short {name}')
print('Validated XR SysCfg: WMac, WSKU, WCAL, BMac present (values not printed)')
PY

chmod 600 "$tmp"
mv -n -- "$tmp" "$OUT"
[[ -s "$OUT" ]] || { echo "failed to save SysCfg" >&2; exit 1; }
echo "Saved private SysCfg to $OUT (mode 0600)"
