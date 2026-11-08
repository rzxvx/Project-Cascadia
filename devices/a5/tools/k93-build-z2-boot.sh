#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test ! -e build/k93/z2-boot-v1
mkdir build/k93/z2-boot-v1
arm-linux-gnueabihf-gcc -static -Os -s -Wall -Wextra -Werror \
    tools/k93-z2-boot.c -o build/k93/z2-boot-v1/k93-z2-boot
sha256sum build/k93/z2-boot-v1/k93-z2-boot
