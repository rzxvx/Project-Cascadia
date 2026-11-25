#!/bin/sh
# render-check.sh [NAME ...]: build tools/gpu/g9p-render (static aarch64) and
# isa/render-check.py's tests, run them on the iPad, compare the pixels.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
A10="$(cd "$HERE/../.." && pwd)"
HOST=${HOST:-root@172.16.43.1}
B="$A10/build/gpu"
mkdir -p "$A10/build/bin"
docker run --rm --platform linux/arm64 -v "$A10:/a10" -v cascadia-a10-ksrc:/src:ro alpine:3.24 sh -c \
  'apk add --no-cache gcc musl-dev linux-headers >/dev/null &&
   mkdir -p /tmp/inc/drm && cp /src/linux/include/uapi/drm/drm.h /src/linux/include/uapi/drm/drm_mode.h \
     /src/linux/include/uapi/drm/pvr_drm.h /tmp/inc/drm/ &&
   gcc -O2 -static -Wall -D__user= -I/tmp/inc -o /a10/build/bin/g9p-render /a10/tools/gpu/g9p-render.c'
names=$(python3 "$HERE/isa/render-check.py" build)
[ $# -gt 0 ] && names="$*"
ssh "$HOST" 'cat > /run/g9p-render && chmod 755 /run/g9p-render' < "$A10/build/bin/g9p-render"
ssh "$HOST" 'cat > /run/render-check.bin' < "$B/render-check.bin"
fails=0
for n in $names; do
    ssh -n "$HOST" "/run/g9p-render /run/render-check.bin $n /run/$n.raw" || { fails=$((fails + 1)); continue; }
    scp -q "$HOST:/run/$n.raw" "$B/render-$n.raw"
    python3 "$HERE/isa/render-check.py" check "$n" "$B/render-$n.raw" || fails=$((fails + 1))
done
echo "render-check: $fails failed"
