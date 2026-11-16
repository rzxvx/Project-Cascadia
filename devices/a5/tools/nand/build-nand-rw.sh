#!/usr/bin/env bash
# Writable NAND root boot bundle, built from the kernel main runs today.
#
#   MAIN=~/Desktop/cascadia-main/devices/a5 bash tools/nand/build-nand-rw.sh
#
# Main's kernel tree and base initramfs are cloned (APFS clones on macOS) into
# build/linux-nand-rw and build/nand-rw-initramfs; main is not touched.  The
# clone gets this branch's NAND drivers built in with their write paths
# (config/p105ap-nand-rw.config), stage 1 gets the NAND-root hook
# (initramfs/sbin/p105-nand-root, sourced just before the NFS block of
# main's init), and the device tree drops the raw h2fmi node for the PPN one
# and boots root=/dev/ftl0p2 with the write gates open for "Linux" only.
# The bundle carries main's local layer (build/overlay) as ./cascadia a5 build
# does, so the NFS / RAM roots stay the fallback.
#
# Output: build/nand-rw-boot/{staging-bundle.bin,staging-loader.bin,...}.
# Boot it: FLASH_OUT=$PWD/build/nand-rw-boot ./cascadia a5 flash [--kdfu] (main).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MAIN="$(cd "${MAIN:?set MAIN to cascadia-main/devices/a5}" && pwd)"
TREE="$ROOT/build/linux-nand-rw"
RAM="$ROOT/build/nand-rw-initramfs"
OUT="$ROOT/build/nand-rw-boot"
IMAGE="${IMAGE:-cascadia-build}"
clone() { if [ "$(uname -s)" = Darwin ]; then cp -cRp "$1" "$2"; else cp -a "$1" "$2"; fi; }

[ -f "$MAIN/build/linux/vmlinux" ] || { echo "no built kernel in $MAIN/build/linux" >&2; exit 1; }
mkdir -p "$ROOT/build"
[ -d "$TREE" ] || clone "$(cd "$MAIN/build/linux" && pwd -P)" "$TREE"
rm -rf "$RAM"
clone "$MAIN/build/initramfs-root" "$RAM"
python3 - "$MAIN/initramfs/init" "$RAM/init" <<'PY'
import sys
s = open(sys.argv[1]).read()
mark = '# --------------------------------------------------------------- NFS root ---'
assert s.count(mark) == 1, 'NFS block marker not found in main init'
if 'p105-nand-root' in s:            # main's init carries the hook since the merge
    open(sys.argv[2], 'w').write(s); sys.exit(0)
hook = ('# NAND root when the command line asks for it (nand-rw / nand-ro profiles);\n'
        '# anything that fails returns here and the NFS / RAM roots follow.\n'
        'if [ -r /sbin/p105-nand-root ]; then\n'
        '    . /sbin/p105-nand-root\n'
        '    p105_try_nand_root || msg "P105: NAND root unavailable; trying recovery roots"\n'
        'fi\n\n')
open(sys.argv[2], 'w').write(s.replace(mark, hook + mark))
PY
cp "$ROOT/initramfs/sbin/p105-nand-root" "$RAM/sbin/p105-nand-root"
chmod 755 "$RAM/init" "$RAM/sbin/p105-nand-root"
mkdir -p "$OUT"

docker run --rm -i -e JOBS="${JOBS:-8}" -v "$ROOT":/branch:ro -v "$MAIN":/main:ro \
    -v "$TREE":/kernel -v "$OUT":/out -v "$RAM":/initramfs:ro "$IMAGE" bash -s <<'BUILD'
set -euo pipefail
export ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf-
rm -rf /kernel/drivers/mtd/nand/apple
cp -a /branch/patches/files/drivers/mtd/nand/apple /kernel/drivers/mtd/nand/apple
python3 - <<'PY'
from pathlib import Path
p = Path('/kernel/drivers/mtd/nand/Kconfig'); s = p.read_text()
line = 'source "drivers/mtd/nand/apple/Kconfig"\n'
if line not in s:
    s = s.replace('source "drivers/mtd/nand/onenand/Kconfig"', line + '\nsource "drivers/mtd/nand/onenand/Kconfig"')
assert s.count(line) == 1; p.write_text(s)
p = Path('/kernel/drivers/mtd/nand/Makefile'); s = p.read_text()
if 'obj-y += apple/\n' not in s: s += '\nobj-y += apple/\n'
p.write_text(s)
PY
printf 'CONFIG_INITRAMFS_SOURCE="/initramfs"\nCONFIG_INITRAMFS_ROOT_UID=-1\nCONFIG_INITRAMFS_ROOT_GID=-1\n' > /tmp/ram.config
/kernel/scripts/kconfig/merge_config.sh -m -O /kernel /kernel/.config \
    /branch/config/p105ap-nand-rw.config /tmp/ram.config > /out/configure.log 2>&1
make -C /kernel olddefconfig >> /out/configure.log 2>&1
for s in APPLE_PPN APPLE_SFTL APPLE_SFTL_WRITE EXT4_FS; do
    grep -qx "CONFIG_$s=y" /kernel/.config || { echo "CONFIG_$s not y" >&2; exit 1; }
done
grep -qx '# CONFIG_MTD_APPLE_H2FMI is not set' /kernel/.config
make -C /kernel -j"$JOBS" zImage > /out/kernel-build.log 2>&1 || { tail -30 /out/kernel-build.log; exit 1; }
cp /kernel/arch/arm/boot/zImage /out/zImage
cp /kernel/.config /out/kernel-config
python3 /main/scripts/cpio-owners.py /kernel/usr/initramfs_data.cpio > /out/cpio-owners.txt
test "$(head -1 /out/cpio-owners.txt)" = '-'
for stamp in pre_spawn umt_go aic1q34 irq_try; do grep -aFq "$stamp" /kernel/vmlinux; done
for stamp in p105_try_nand_root /dev/ftl0p2 'P105: stage1 start' p105-stage2; do
    grep -aFq "$stamp" /kernel/usr/initramfs_data.cpio || { echo "initramfs lacks $stamp" >&2; exit 1; }
done
dtc -I dts -O dtb -i /main/dts /branch/dts/p105ap-nand-rw.dts -o /out/p105ap-nand-rw.dtb 2> /out/dtc.log
args=$(fdtget /out/p105ap-nand-rw.dtb /chosen bootargs)
args=$(python3 - "$args" <<'PY'
import sys
w = sys.argv[1].split()
assert w.count('root=/dev/ram0') == 1 and w.count('rw') == 1
print(' '.join('root=/dev/ftl0p2' if x == 'root=/dev/ram0' else x for x in w),
      'apple_ppn.hold_probe=1 apple_ppn.allow_write=1 apple_ftl.auto_mount=0',
      'apple_ftl.enable_write=1 apple_ftl.write_volume=Linux')
PY
)
fdtput -t s /out/p105ap-nand-rw.dtb /chosen bootargs "$args"
printf '%s\n' "$args" > /out/bootargs.txt
test "$(fdtget /out/p105ap-nand-rw.dtb /soc@30000000/flash-controller@1200000 status)" = disabled
test "$(fdtget /out/p105ap-nand-rw.dtb /soc@30000000/ppn@1200000 compatible)" = apple,s5l8940x-ppn
cat /out/zImage /out/p105ap-nand-rw.dtb > /out/zImage-dtb
rm -rf /out/loader-src; mkdir -p /out/loader-src
cp /main/pongo/linux_boot_start.S /main/pongo/linux_boot.c /main/pongo/linux-boot.ld \
   /main/pongo/fb_text8.h /main/pongo/font8x8.h /out/loader-src/
python3 - <<'PY'
from pathlib import Path
import struct
z = Path('/out/zImage-dtb').read_bytes()
off = z.rfind(bytes.fromhex('d00dfeed')); tot = struct.unpack_from('>I', z, off + 4)[0]
va = 0x80008000 + off
Path('/out/loader-src/staging-generated.h').write_text(
    '#ifndef STAGING_GENERATED_H\n#define STAGING_GENERATED_H\n'
    '#define DTB_PHYS %#xu\n#define DTB_SIZE %#xu\n#endif\n' % (va, tot))
Path('/out/loader-src/staging-generated.inc').write_text(
    '\t.equ\tDTB_PHYS, %#x\n\t.equ\tDTB_SIZE, %#x\n' % (va, tot))
PY
arm-linux-gnueabihf-gcc -static -nostdlib -nostartfiles -O2 -I /out/loader-src \
    -fno-builtin -ffreestanding -Wl,--build-id=none \
    -Wl,-T,/out/loader-src/linux-boot.ld -o /out/linux-boot.elf \
    /out/loader-src/linux_boot_start.S /out/loader-src/linux_boot.c
arm-linux-gnueabihf-objcopy -O binary /out/linux-boot.elf /out/linux-boot.bin
BUILD

python3 "$MAIN/scripts/assemble.py" "$OUT/linux-boot.bin" "$OUT/zImage-dtb" \
    "$MAIN/build/overlay" "$OUT/staging-bundle.bin"
cp "$OUT/linux-boot.bin" "$OUT/staging-loader.bin"
( cd "$OUT" && shasum -a 256 zImage-dtb staging-bundle.bin staging-loader.bin kernel-config > SHA256SUMS )
cat "$OUT/bootargs.txt"
echo "nand-rw bundle: $OUT (not booted yet)"
