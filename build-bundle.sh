#!/bin/bash
set -e

ROOT=/work
OUT=/work/output
ZIMAGE="$OUT/zImage-dtb"
BUNDLE="$OUT/staging-bundle.bin"

# Найти DTB в zImage-dtb и сгенерировать staging-generated.h
python3 - "$ZIMAGE" "$ROOT/pongo/staging-generated.h" <<'PY'
import struct, sys
from pathlib import Path
z, inc = Path(sys.argv[1]), Path(sys.argv[2])
d = z.read_bytes()
off = d.rfind(bytes.fromhex("d00dfeed"))
if off < 0:
    raise SystemExit("zImage-dtb has no DTB magic")
tot = struct.unpack_from(">I", d, off + 4)[0]
va = 0x80008000 + off
inc.write_text(
    f"/* Auto-generated */\n"
    f"#ifndef STAGING_GENERATED_H\n#define STAGING_GENERATED_H\n"
    f"#define DTB_PHYS {va:#x}u\n"
    f"#define DTB_SIZE {tot:#x}u\n"
    f"#endif\n"
)
print(f"DTB_PHYS {va:#x}  DTB_SIZE {tot:#x}  offset {off:#x}")
PY

# То же для .inc
python3 - "$ZIMAGE" "$ROOT/pongo/staging-generated.inc" <<'PY'
import struct, sys
from pathlib import Path
z, inc = Path(sys.argv[1]), Path(sys.argv[2])
d = z.read_bytes()
off = d.rfind(bytes.fromhex("d00dfeed"))
tot = struct.unpack_from(">I", d, off + 4)[0]
va = 0x80008000 + off
inc.write_text(f"\t.equ\tDTB_PHYS, {va:#x}\n\t.equ\tDTB_SIZE, {tot:#x}\n")
PY

# Собираем linux-boot loader
arm-linux-gnueabihf-gcc -static -nostdlib -nostartfiles -O2 \
    -I "$ROOT/pongo" \
    -fno-builtin -ffreestanding \
    -Wl,--build-id=none \
    -Wl,-T,"$ROOT/pongo/linux-boot.ld" \
    -o "$OUT/linux-boot.elf" \
    "$ROOT/pongo/linux_boot_start.S" \
    "$ROOT/pongo/linux_boot.c"

arm-linux-gnueabihf-objcopy -O binary "$OUT/linux-boot.elf" "$OUT/linux-boot.bin"

lsz=$(stat -c%s "$OUT/linux-boot.bin")
echo "loader: ${lsz} bytes"

# Собираем bundle: loader + pad to 0x8000 + zImage-dtb
cp "$OUT/linux-boot.bin" "$BUNDLE"
pad=$((32768 - lsz))
dd if=/dev/zero bs=1 count=$pad status=none >> "$BUNDLE"
cat "$ZIMAGE" >> "$BUNDLE"

echo "staging-bundle.bin готов: $(stat -c%s "$BUNDLE") bytes"
