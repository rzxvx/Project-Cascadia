#!/bin/bash
set -e

KERNEL_DIR=/root/linux
KERNEL_TAG="v6.6"

echo "=== 1. Cloning Kernel ==="
if [ ! -d "$KERNEL_DIR" ]; then
    git clone --depth 1 --branch "$KERNEL_TAG" https://github.com/torvalds/linux.git "$KERNEL_DIR"
else
    echo "Уже склонировано, пропускаем"
fi

cd "$KERNEL_DIR"

echo "=== 2.(mach-apple5) ==="
mkdir -p arch/arm/mach-apple5
cp /work/mach-apple5/apple5.c arch/arm/mach-apple5/
cp /work/mach-apple5/Kconfig arch/arm/mach-apple5/
cp /work/mach-apple5/Makefile arch/arm/mach-apple5/

if ! grep -q "mach-apple5" arch/arm/Makefile; then
    echo 'machine-$(CONFIG_ARCH_APPLE_S5L8942X) += apple5' >> arch/arm/Makefile
fi
if ! grep -q "mach-apple5/Kconfig" arch/arm/Kconfig; then
    sed -i '/^source "arch\/arm\/mach-highbank\/Kconfig"/i source "arch/arm/mach-apple5/Kconfig"' arch/arm/Kconfig
fi

echo "=== 3. Copying Device Tree ==="
cp /work/ipad-mini1.dts arch/arm/boot/dts/
if ! grep -q "ipad-mini1.dtb" arch/arm/boot/dts/Makefile; then
    echo 'dtb-$(CONFIG_ARCH_APPLE_S5L8942X) += ipad-mini1.dtb' >> arch/arm/boot/dts/Makefile
fi

echo "=== 4. Kernel Configuring ==="
make multi_v7_defconfig

scripts/config --enable CONFIG_ARCH_APPLE_S5L8942X
scripts/config --enable CONFIG_SERIAL_SAMSUNG
scripts/config --enable CONFIG_SERIAL_SAMSUNG_CONSOLE
scripts/config --enable CONFIG_SERIAL_EARLYCON
scripts/config --enable CONFIG_FB
scripts/config --enable CONFIG_FB_SIMPLE
scripts/config --enable CONFIG_FRAMEBUFFER_CONSOLE
scripts/config --enable CONFIG_FRAMEBUFFER_CONSOLE_DETECT_PRIMARY
scripts/config --enable CONFIG_VT
scripts/config --enable CONFIG_VT_CONSOLE
scripts/config --enable CONFIG_DUMMY_CONSOLE
scripts/config --enable CONFIG_CACHE_L2X0
scripts/config --set-str CONFIG_CMDLINE "earlycon=exynos4210,mmio32,0x32500000 console=ttySAC0,115200n8 console=tty0 loglevel=8 ignore_loglevel"
scripts/config --enable CONFIG_CMDLINE_FORCE

make olddefconfig

echo "=== 5. Build ==="
make -j$(nproc) zImage dtbs 2>&1 | tee /work/build.log

echo "=== 6. Copying results ==="
mkdir -p /work/output
cp arch/arm/boot/zImage /work/output/
cp arch/arm/boot/dts/ipad-mini1.dtb /work/output/

echo "===DONE==="
echo "zImage и dtb в /work/output/"
echo "Лог сборки: /work/build.log"