#!/usr/bin/env bash
set -euo pipefail
A12="$(cd "$(dirname "$0")/.." && pwd)"
K="$A12/build/linux"
DOCS="$A12/build/hoolock-docs"
OUT="$A12/build/payload"
[ -d "$K/.git" ] || { echo "missing $K" >&2; exit 1; }

# Apply the complete reviewed XR patch series in filename order. The old
# worktree already had all patches applied; a fresh pinned checkout does not.
# An already-applied patch is accepted, but any other mismatch stops the build.
for patch in "$A12"/linux/patches/*.patch; do
  if git -C "$K" apply --check "$patch" 2>/dev/null; then
    git -C "$K" apply "$patch"
  elif ! git -C "$K" apply --reverse --check "$patch" 2>/dev/null; then
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
cp "$DOCS/config_16k" "$K/.config"
cd "$K"
# One kernel for both boards. H11P display, apple-mt-spi touch, hci_bcm4377,
# the SoC temperature sensors and agx are the XS Max's; t8020-n841.dts keeps
# their nodes off and the others bind nothing on the XR.
./scripts/config --disable CONFIG_BLK_DEV_NBD \
  --disable CONFIG_USB_G_MULTI \
  --enable CONFIG_ARM_APPLE_SOC_CPUFREQ --enable CONFIG_CPU_THERMAL \
  --enable CONFIG_USB_CONFIGFS --enable CONFIG_USB_CONFIGFS_ACM \
  --enable CONFIG_USB_CONFIGFS_ECM --enable CONFIG_USB_DWC2 \
  --enable CONFIG_USB_DWC2_DUAL_ROLE --enable CONFIG_PHY_APPLE_AUSB \
  --enable CONFIG_INPUT_TOUCHSCREEN \
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
  --enable CONFIG_DRM_SIMPLEDRM --enable CONFIG_DEVTMPFS --enable CONFIG_DEVTMPFS_MOUNT \
  --enable CONFIG_WIRELESS --enable CONFIG_DRM_APPLE_H11P \
  --enable CONFIG_TOUCHSCREEN_APPLE_MT_SPI --enable CONFIG_BT_HCIBCM4377 \
  --enable CONFIG_SENSORS_APPLE_SOC_TEMP --enable CONFIG_DRM_AGX
make ARCH=arm64 LLVM=1 olddefconfig
make ARCH=arm64 LLVM=1 -j"${JOBS:-8}" Image.gz
mkdir -p "$OUT"
cp arch/arm64/boot/Image.gz "$OUT/Image.gz"
cp .config "$A12/build/kernel.config"
sha256sum "$OUT/Image.gz"
