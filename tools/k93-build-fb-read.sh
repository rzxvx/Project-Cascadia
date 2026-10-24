#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test ! -e build/k93/fb-read-v1
mkdir build/k93/fb-read-v1
arm-linux-gnueabihf-gcc -static -Os -s -Wall -Wextra -Werror \
    tools/k93-fb-read.c -o build/k93/fb-read-v1/k93-fb-read
sha256sum build/k93/fb-read-v1/k93-fb-read
