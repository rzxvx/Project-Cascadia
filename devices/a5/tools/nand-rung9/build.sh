#!/bin/sh
# Ladder rung 9 image: the main kernel and loader of rung 1 (ppn DTB), and an
# overlay = this machine's build/overlay + /init with a NAND-root branch +
# the driver modules in /lib/nand + /etc/nandroot (its content: verify_every).
#   tools/nand-rung9/build.sh [verify_every]
# Writes output/rung9/staging-bundle.bin; boot with
#   FLASH_OUT=<this>/output/rung9 ./cascadia a5 flash
set -eu
C=$(cd "$(dirname "$0")/../.." && pwd)
M=${MAIN_A5:-$HOME/Desktop/cascadia-main/devices/a5}
O=$C/output/rung9
rm -rf "$O"; mkdir -p "$O/overlay/lib/nand"
cp -a "$M/build/overlay/." "$O/overlay/"
install -m 755 "$C/tools/nand-rung9/init" "$O/overlay/init"
cp "$C/output/rung8/apple_ppn.ko" "$C/output/rung8/apple_ftl.ko" "$O/overlay/lib/nand/"
echo "${1:-16}" > "$O/overlay/etc/nandroot"
cp "$C/output/rung1/zImage-dtb" "$C/output/rung1/linux-boot.bin" \
   "$C/output/rung1/staging-loader.bin" "$O/"
python3 "$M/scripts/assemble.py" "$O/linux-boot.bin" "$O/zImage-dtb" "$O/overlay" \
    "$O/staging-bundle.bin"
