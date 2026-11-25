#!/bin/sh
# Build tools/gpu/g9p-probe.c (static aarch64, build/bin/) and the probe
# kernels (build/gpu/g9p-probe.bin), push both to the iPad's /run; with
# arguments, run g9p-probe there with them.
set -e
A10="$(cd "$(dirname "$0")/../.." && pwd)"
HOST=${HOST:-root@172.16.43.1}
mkdir -p "$A10/build/bin"
python3 "$A10/tools/gpu/g9p-probe-shaders.py" >/dev/null
docker run --rm --platform linux/arm64 -v "$A10:/a10" -v cascadia-a10-ksrc:/src:ro alpine:3.24 sh -c \
  'apk add --no-cache gcc musl-dev linux-headers >/dev/null &&
   mkdir -p /tmp/inc/drm && cp /src/linux/include/uapi/drm/drm.h /src/linux/include/uapi/drm/drm_mode.h \
     /src/linux/include/uapi/drm/pvr_drm.h /tmp/inc/drm/ &&
   gcc -O2 -static -Wall -D__user= -I/tmp/inc -o /a10/build/bin/g9p-probe /a10/tools/gpu/g9p-probe.c'
ssh -o ConnectTimeout=8 "$HOST" 'cat > /run/g9p-probe && chmod 755 /run/g9p-probe' < "$A10/build/bin/g9p-probe"
ssh -o ConnectTimeout=8 "$HOST" 'cat > /run/g9p-probe.bin' < "$A10/build/gpu/g9p-probe.bin"
[ $# -gt 0 ] && ssh -o ConnectTimeout=8 "$HOST" /run/g9p-probe "$@"
exit 0
