#!/bin/sh
# tools/task-killer/build.sh -- task-killer for the iPad 7: static aarch64,
# built in an Alpine container (the glyphs by devices/a5/tools/screenpad's
# stb_truetype.h). build rootfs and rootfs sync put it on the iPad as
# /usr/local/bin/task-killer; a10-init starts it.
# Output: devices/a10/build/task-killer/task-killer
set -eu
D="$(cd "$(dirname "$0")" && pwd)"
A10="$(cd "$D/../.." && pwd)"
OUT="$A10/build/task-killer"
mkdir -p "$OUT"
docker run --rm --platform linux/arm64 -v "$A10/..":/devices -w /devices alpine:3.24 sh -ec '
    apk add -q build-base linux-headers >/dev/null
    gcc -static -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
        -Wno-missing-field-initializers -I a5/tools/screenpad \
        -o a10/build/task-killer/task-killer a10/tools/task-killer/task-killer.c -lm'
ls -l "$OUT/task-killer"
