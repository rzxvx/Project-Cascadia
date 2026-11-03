#!/usr/bin/env bash
# Offline build only. This command never runs the helper or contacts USB.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/k93/user-watchdog
arm-linux-gnueabihf-as -o build/k93/user-watchdog/reset.o pongo/k93_user_watchdog.S
arm-linux-gnueabihf-ld -s --build-id=none -T pongo/k93_user_watchdog.ld \
    -o build/k93/user-watchdog/k93-watchdog build/k93/user-watchdog/reset.o
python3 - <<'PY'
from pathlib import Path
import struct
data = Path('build/k93/user-watchdog/k93-watchdog').read_bytes()
assert len(data) <= 2048 and data[:7] == b'\x7fELF\x01\x01\x01'
assert struct.unpack_from('<H', data, 18)[0] == 40  # ARM
offset = struct.unpack_from('<I', data, 28)[0]
assert struct.unpack_from('<H', data, 44)[0] == 1
kind, fileoff, va, _, filesz, memsz, flags, _ = struct.unpack_from('<8I', data, offset)
assert (kind, fileoff, va, flags) == (1, 0, 0x10000, 5)
assert filesz == memsz and filesz <= len(data)
print('Static K93-only ELF validated; no device action performed')
PY
sha256sum build/k93/user-watchdog/k93-watchdog
