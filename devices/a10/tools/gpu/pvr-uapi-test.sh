#!/bin/sh
# Build tools/gpu/pvr-uapi-test.c as a static aarch64 binary (build/bin/), with
# the UAPI headers of the kernel tree in the cascadia-a10-ksrc volume, and run
# it on the iPad (HOST, default root@172.16.43.1) when "run" is given.
set -e
A10="$(cd "$(dirname "$0")/../.." && pwd)"
HOST=${HOST:-root@172.16.43.1}
mkdir -p "$A10/build/bin"
docker run --rm --platform linux/arm64 -v "$A10:/a10" -v cascadia-a10-ksrc:/src:ro alpine:3.24 sh -c \
  'apk add --no-cache gcc musl-dev linux-headers >/dev/null &&
   mkdir -p /tmp/inc/drm && cp /src/linux/include/uapi/drm/drm.h /src/linux/include/uapi/drm/drm_mode.h \
     /src/linux/include/uapi/drm/pvr_drm.h /tmp/inc/drm/ &&
   gcc -O2 -static -Wall -D__user= -I/tmp/inc -o /a10/build/bin/pvr-uapi-test /a10/tools/gpu/pvr-uapi-test.c'
if [ "${1:-}" = run ]; then
    ssh -o ConnectTimeout=8 "$HOST" 'cat > /run/pvr-uapi-test && chmod 755 /run/pvr-uapi-test && /run/pvr-uapi-test' \
        < "$A10/build/bin/pvr-uapi-test"
fi
