#!/usr/bin/env bash
set -euo pipefail
A12="$(cd "$(dirname "$0")/.." && pwd)"
K="$A12/build/linux"
DOCS="$A12/build/hoolock-docs"
OUT="$A12/build/payload"
[ -d "$K/.git" ] || { echo "missing $K" >&2; exit 1; }

# Migrate only the exact, known previous SIO-overwritten SPI source. This
# restores one source file for incremental builds, never the object tree.
# Unknown edits still fail the normal patch/reverse-patch checks below.
if [ -f "$K/drivers/spi/spi-apple.c" ] &&
   [ "$(sha256sum "$K/drivers/spi/spi-apple.c" | cut -d ' ' -f1)" = 6861343fc5234a08e45fc2341e4422c5e72033a1fd66b0f6f043dfffa2d23ae2 ]; then
    git -C "$K" show HEAD:drivers/spi/spi-apple.c > "$K/drivers/spi/spi-apple.c.baseline"
    mv "$K/drivers/spi/spi-apple.c.baseline" "$K/drivers/spi/spi-apple.c"
fi

# Apply the complete reviewed XR patch series in filename order. The old
# worktree already had all patches applied; a fresh pinned checkout does not.
# An already-applied patch is accepted, but any other mismatch stops the build.
for patch in "$A12"/linux/patches/*.patch; do
  if git -C "$K" apply --check "$patch" 2>/dev/null; then
    git -C "$K" apply "$patch"
  elif ! git -C "$K" apply --reverse --check "$patch" 2>/dev/null; then
    # The AGX source is occasionally edited/generated in the ignored build
    # tree after its patch was applied, so a strict reverse check no longer
    # matches byte-for-byte.  Accept that incremental state only when both
    # integration markers and the complete source directory are present.
    if [[ "$(basename "$patch")" == linux-0020-agx.patch &&
          -f "$K/drivers/gpu/drm/agx/agx_drv.c" ]] &&
       grep -q 'source "drivers/gpu/drm/agx/Kconfig"' \
         "$K/drivers/gpu/drm/Kconfig" &&
       grep -q 'obj-$(CONFIG_DRM_AGX).*agx/' "$K/drivers/gpu/drm/Makefile"; then
      continue
    fi
    echo "patch neither applies nor is already present: $patch" >&2
    exit 1
  fi
done

# XR/N841 HBPP + HIDSPI touchscreen.  Keep this outside the downloaded kernel
# tree as the auditable port source, then stage it before each build.
cp "$A12/linux/extras/apple_hidspi_touch.c" \
   "$K/drivers/input/touchscreen/apple_hidspi_touch.c"
grep -q 'apple_hidspi_touch.o' "$K/drivers/input/touchscreen/Makefile" || \
  printf 'obj-$(CONFIG_SPI_MASTER) += apple_hidspi_touch.o\n' >> \
    "$K/drivers/input/touchscreen/Makefile"
cp "$A12/linux/extras/apple_z2_j171.c" \
   "$K/drivers/input/touchscreen/apple_z2_j171.c"
grep -q 'apple_z2_j171.o' "$K/drivers/input/touchscreen/Makefile" || \
  printf 'obj-$(CONFIG_SPI_MASTER) += apple_z2_j171.o\n' >> \
    "$K/drivers/input/touchscreen/Makefile"

# Real T8020 PMGR touch reference; only J171 DT instantiates this provider.
cp "$A12/linux/extras/clk-apple-t8020-touch.c" \
   "$K/drivers/clk/clk-apple-t8020-touch.c"
grep -q 'clk-apple-t8020-touch.o' "$K/drivers/clk/Makefile" || \
  printf 'obj-$(CONFIG_ARCH_APPLE) += clk-apple-t8020-touch.o\n' >> \
    "$K/drivers/clk/Makefile"

# J171 firmware upload uses the ASCv2/RTKit T8020 SmartIO coprocessor for
# SPI3 TX DMA.  This is the upstream Asahi SIO driver, extended with finite
# slave-SG transfers for the touchscreen firmware upload path.
cp "$A12/linux/extras/apple-sio-core.c" \
   "$K/drivers/dma/apple-sio.c"
# Keep the XR legacy implementation produced by linux-0010 in spi-apple.c.
# The DMA/runtime-PM implementation has a distinct J171-only compatible/name.
if ! cmp -s "$A12/linux/extras/spi-apple-sio.c" "$K/drivers/spi/spi-apple-j171.c"; then
    cp "$A12/linux/extras/spi-apple-sio.c" "$K/drivers/spi/spi-apple-j171.c"
fi
grep -q 'spi-apple-j171.o' "$K/drivers/spi/Makefile" || \
    printf 'obj-$(CONFIG_SPI_APPLE) += spi-apple-j171.o\n' >> "$K/drivers/spi/Makefile"

# Remove the superseded split T8010 driver wiring from an incremental tree.
sed -i '/^obj-$(CONFIG_APPLE_SIO) += apple-sio.o$/d' \
  "$K/drivers/soc/apple/Makefile"
sed -i '/^apple-sio-y := apple-sio-core.o$/d' \
  "$K/drivers/soc/apple/Makefile"
python3 - "$K/drivers/soc/apple/Kconfig" "$K/drivers/dma/Kconfig" <<'PY'
from pathlib import Path
import sys

old_core = '''\nconfig APPLE_SIO
\ttristate "Apple T8010/T8020 SmartIO coprocessor"
\tdepends on ARM64
\tdepends on PM
\tdepends on ARCH_APPLE || COMPILE_TEST
\thelp
\t  Enable the SmartIO firmware and mailbox transport used by DMA clients
\t  on Apple A-series SoCs.
'''
old_dma = '''\nconfig APPLE_SIO_DMA
\ttristate "Apple T8010/T8020 SmartIO DMA support"
\tdepends on APPLE_SIO
\tselect DMA_ENGINE
\tselect DMA_VIRTUAL_CHANNELS
\thelp
\t  Expose SmartIO peripheral DMA endpoints through DMAengine.
'''
for name, block in zip(sys.argv[1:], (old_core, old_dma)):
    path = Path(name)
    path.write_text(path.read_text().replace(block, ""))
PY

grep -q '^config APPLE_SIO$' "$K/drivers/dma/Kconfig" || cat >> \
  "$K/drivers/dma/Kconfig" <<'EOF'

config APPLE_SIO
	tristate "Apple RTKit SmartIO DMA support"
	depends on ARCH_APPLE || COMPILE_TEST
	select DMA_ENGINE
	select DMA_VIRTUAL_CHANNELS
	help
	  Enable the RTKit-backed SmartIO DMA controller found on recent Apple
	  SoCs, including the T8020 in J171 iPads.
EOF
grep -q '^obj-$(CONFIG_APPLE_SIO)' "$K/drivers/dma/Makefile" || \
  printf 'obj-$(CONFIG_APPLE_SIO) += apple-sio.o\n' >> \
    "$K/drivers/dma/Makefile"
cp "$DOCS/config_16k" "$K/.config"
cd "$K"
./scripts/config --disable CONFIG_BLK_DEV_NBD \
  --disable CONFIG_USB_G_MULTI \
  --enable CONFIG_ARM_APPLE_SOC_CPUFREQ --enable CONFIG_CPU_THERMAL \
  --enable CONFIG_USB_CONFIGFS --enable CONFIG_USB_CONFIGFS_ACM \
  --enable CONFIG_USB_CONFIGFS_ECM --enable CONFIG_USB_DWC2 \
  --enable CONFIG_USB_DWC2_DUAL_ROLE --enable CONFIG_PHY_APPLE_AUSB \
  --enable CONFIG_INPUT_TOUCHSCREEN \
  --enable CONFIG_DMADEVICES --enable CONFIG_APPLE_SIO \
  --disable CONFIG_APPLE_SIO_DMA \
  --enable CONFIG_CFG80211 --disable CONFIG_MAC80211 \
  --enable CONFIG_WLAN --enable CONFIG_WLAN_VENDOR_BROADCOM \
  --enable CONFIG_BRCMUTIL --enable CONFIG_BRCMFMAC \
  --enable CONFIG_BRCMFMAC_PCIE --enable CONFIG_BRCMDBG \
  --enable CONFIG_BT --enable CONFIG_BT_BREDR --enable CONFIG_BT_LE \
  --enable CONFIG_BT_BCM --enable CONFIG_BT_HCIUART \
  --enable CONFIG_BT_HCIUART_SERDEV --enable CONFIG_BT_HCIUART_H4 \
  --enable CONFIG_BT_HCIUART_BCM \
  --enable CONFIG_NETWORK_FILESYSTEMS --enable CONFIG_NFS_FS \
  --enable CONFIG_NFS_V3 --enable CONFIG_NFS_V4 --enable CONFIG_ROOT_NFS \
  --disable CONFIG_FW_LOADER_USER_HELPER \
  --disable CONFIG_FW_LOADER_USER_HELPER_FALLBACK \
  --enable CONFIG_DRM_SIMPLEDRM --enable CONFIG_DEVTMPFS --enable CONFIG_DEVTMPFS_MOUNT
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j"${JOBS:-1}" LD="ld.lld --threads=${LINK_JOBS:-1}" Image.gz
mkdir -p "$OUT"
cp arch/arm64/boot/Image.gz "$OUT/Image.gz"
cp .config "$A12/build/kernel.config"
sha256sum "$OUT/Image.gz"
