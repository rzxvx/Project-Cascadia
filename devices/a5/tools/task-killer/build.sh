#!/bin/sh
# tools/task-killer/build.sh [install] -- task-killer for the iPad: static
# ARM, built in the cascadia-build image into build/task-killer/ (the glyphs
# by tools/screenpad's stb_truetype.h).  "install": onto the iPad
# (DEVICE=root@10.55.0.2) as /usr/local/bin/task-killer, and started there
# again (START=0: not, as for the root ./cascadia a5 desktop-root builds);
# stage 2 starts it at every boot from then on.
set -eu
D="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$D/../.." && pwd)"
DEVICE="${DEVICE:-root@10.55.0.2}"
OUT="$ROOT/build/task-killer"
mkdir -p "$OUT"
docker run --rm -v "$ROOT":/cascadia -w /cascadia cascadia-build \
    arm-linux-gnueabihf-gcc -static -O2 -Wall -Wextra -Wno-unused-parameter \
        -Wno-sign-compare -Wno-missing-field-initializers \
        -I patches/files/include/uapi/drm -I /usr/arm-linux-gnueabihf/include/drm \
        -I tools/screenpad \
        -o build/task-killer/task-killer tools/task-killer/task-killer.c -lm
ls -l "$OUT/task-killer"
if [ "${1:-}" = install ]; then
    ssh "$DEVICE" 'mkdir -p /usr/local/bin &&
                   cat > /usr/local/bin/task-killer.new && chmod 755 /usr/local/bin/task-killer.new &&
                   mv /usr/local/bin/task-killer.new /usr/local/bin/task-killer' < "$OUT/task-killer"
    [ "${START:-1}" = 0 ] || ssh "$DEVICE" '{ pkill -x task-killer; sleep 0.5;
                     setsid /usr/local/bin/task-killer >/tmp/task-killer.log 2>&1 </dev/null & }'
    echo "installed on $DEVICE and started: /usr/local/bin/task-killer (log /tmp/task-killer.log)"
fi
