#!/bin/bash
set -e

KERNEL_DIR=/kernel
KERNEL_TAG="v6.12"

echo "=== 1. Cloning Kernel ==="
if [ ! -d "$KERNEL_DIR" ]; then
    git clone --depth 1 --branch "$KERNEL_TAG" https://github.com/torvalds/linux.git "$KERNEL_DIR"
else
    echo "Уже склонировано, пропускаем"
fi

cd "$KERNEL_DIR"

echo "=== 2. Applying patches ==="
python3 /work/scripts/apply-kernel-patches.py --tree "$KERNEL_DIR"

echo "=== 3. Copying Device Tree ==="
cp /work/ipad-mini1.dts arch/arm/boot/dts/
if ! grep -q "ipad-mini1.dtb" arch/arm/boot/dts/Makefile; then
    echo 'dtb-$(CONFIG_ARCH_APPLE_S5L) += ipad-mini1.dtb' >> arch/arm/boot/dts/Makefile
fi

echo "=== 4. Kernel Configuring ==="
make multi_v7_defconfig
scripts/kconfig/merge_config.sh -m .config /work/config/p105ap.config
scripts/config --set-str CONFIG_CMDLINE "earlycon=s3c6400,0x32500000 keep_bootcon console=tty0 lpj=5000000 loglevel=8 ignore_loglevel panic=10"
scripts/config --enable CONFIG_CMDLINE_FORCE
scripts/config --enable CONFIG_GENERIC_IRQ_IPI_MUX
scripts/config --enable CONFIG_APPLE_AIC1
scripts/config --enable CONFIG_GENERIC_IRQ_IPI_MUX
make olddefconfig

echo "=== 5. Build ==="
make -j$(nproc) zImage dtbs 2>&1 | tee /work/build.log

echo "=== 6. Copying results ==="
mkdir -p /work/output
cp arch/arm/boot/zImage /work/output/
cp arch/arm/boot/dts/ipad-mini1.dtb /work/output/

echo "===DONE==="
