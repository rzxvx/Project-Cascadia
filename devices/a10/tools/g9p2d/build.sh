#!/bin/sh
# Build g9p2d (tools/g9p2d) for the iPad's Alpine (aarch64, musl) in docker:
# build/g9p2d/{g9p2d-test, libg9psdl.so, g9p2d.bin}; "install" puts them on the
# iPad (HOST, default root@172.16.43.1) under /usr/local/lib/g9p2d, with
# supertux-gpu in /usr/local/bin; "test [ARGS]" runs g9p2d-test there.
set -e
A10="$(cd "$(dirname "$0")/../.." && pwd)"
HOST=${HOST:-root@172.16.43.1}
OUT="$A10/build/g9p2d"
mkdir -p "$OUT"
python3 "$A10/tools/g9p2d/g9p2d-shaders.py"
cp "$A10/build/gpu/g9p2d.bin" "$OUT/"
docker run --rm --platform linux/arm64 -v "$A10:/a10" -v cascadia-a10-ksrc:/src:ro alpine:3.24 sh -c \
  'apk add --no-cache gcc musl-dev linux-headers sdl2-dev >/dev/null &&
   mkdir -p /tmp/inc/drm && cp /src/linux/include/uapi/drm/drm.h /src/linux/include/uapi/drm/drm_mode.h \
     /src/linux/include/uapi/drm/pvr_drm.h /tmp/inc/drm/ &&
   cd /a10/tools/g9p2d &&
   gcc -O2 -Wall -D__user= -I/tmp/inc -o /a10/build/g9p2d/g9p2d-test g9p2d-test.c g9p2d.c -lm &&
   if [ -f g9psdl.c ]; then
     gcc -O2 -Wall -shared -fPIC -D__user= -I/tmp/inc -o /a10/build/g9p2d/libg9psdl.so g9psdl.c g9p2d.c \
       $(pkg-config --cflags sdl2) -ldl -lm -lpthread; fi'
ipad() { ssh -o ConnectTimeout=8 "$HOST" "$@"; }
case "${1:-}" in
install|test)
    ipad 'mkdir -p /usr/local/lib/g9p2d'
    for f in g9p2d-test g9p2d.bin libg9psdl.so; do
        [ -f "$OUT/$f" ] && ipad "cat > /usr/local/lib/g9p2d/$f" < "$OUT/$f"
    done
    ipad 'chmod 755 /usr/local/lib/g9p2d/g9p2d-test'
    if [ -f "$A10/tools/g9p2d/supertux.sh" ]; then
        ipad 'cat > /usr/local/lib/g9p2d/supertux.sh && chmod 755 /usr/local/lib/g9p2d/supertux.sh && mkdir -p /usr/local/bin &&
              ln -sf /usr/local/lib/g9p2d/supertux.sh /usr/local/bin/supertux-gpu' < "$A10/tools/g9p2d/supertux.sh"
    fi
    if [ "$1" = test ]; then shift; ipad /usr/local/lib/g9p2d/g9p2d-test "$@"; fi
    ;;
esac
