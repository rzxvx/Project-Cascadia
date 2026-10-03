#!/usr/bin/env bash
#
# Build tools/sgx/lib for the device without running any ARM code: Ubuntu's
# arm-linux-gnueabihf gcc, pointed at a sysroot of Alpine's own armhf packages
# (musl, its headers, the kernel headers, sdl2-compat) instead of at glibc.
# Runs INSIDE the cascadia-build image (./cascadia gpu), like the rootfs
# steps, and for the same reason (scripts/apk-unpack.py): a Linux host may
# have no ARM emulation, and the result must not depend on it.
#
#   bash tools/sgx/lib/cross.sh    -> sprites demo2 sgxinfo libsgxsdl.so here
#
# The same Makefile also builds natively in an Alpine armv7 container (see
# its header); both give the device the same ABI: EABI hard-float, musl,
# /lib/ld-musl-armhf.so.1.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
SR="$ROOT/build/sgx2d/sysroot"
ALPINE="${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org}/alpine/v3.24"
CROSS=arm-linux-gnueabihf-

if [ ! -f "$SR/.done" ]; then
    echo "==> sysroot: Alpine v3.24 armhf musl, linux-headers, sdl2-compat"
    python3 - "$ROOT/scripts/apk-unpack.py" "$SR" "$ALPINE" <<'EOF'
import importlib.util, os, shutil, sys
apku, sr, base = sys.argv[1:]
spec = importlib.util.spec_from_file_location('apku', apku)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
want = {'main': ['musl', 'musl-dev', 'linux-headers'],
        'community': ['sdl2-compat', 'sdl2-compat-dev']}
shutil.rmtree(sr, ignore_errors=True)
os.makedirs(sr)
for repo, pkgs in want.items():
    url = '%s/%s/armhf' % (base, repo)
    by_name, _ = m.parse_index(m.fetch(url + '/APKINDEX.tar.gz'))
    for p in pkgs:
        print('    ' + by_name[p], flush=True)
        m.untar_apk(m.fetch('%s/%s' % (url, by_name[p])), sr)
EOF
    touch "$SR/.done"
fi

# gcc's own headers (stddef.h, arm_neon.h, ...) but musl's libc headers, and
# musl's start files and libc with gcc's libgcc for the helpers it calls.
GCCINC=$("${CROSS}gcc" -print-file-name=include)
LIBGCC=$("${CROSS}gcc" -print-libgcc-file-name)
# drm.h from the kernel headers, the render node's own header from this tree
CFLAGS="-O2 -Wall -D_FILE_OFFSET_BITS=64 -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard
 -nostdinc -isystem $SR/usr/include -isystem $GCCINC
 -I$SR/usr/include/drm -I$ROOT/patches/files/include/uapi/drm"
LDFLAGS="-nostdlib -L$SR/usr/lib -L$SR/lib -Wl,--dynamic-linker=/lib/ld-musl-armhf.so.1
 -Wl,-rpath-link,$SR/usr/lib -Wl,--hash-style=both"
CRTI="$SR/usr/lib/crti.o"
CRTN="$SR/usr/lib/crtn.o"
cd "$HERE"
set -x
for p in sprites demo2; do
    # shellcheck disable=SC2086
    "${CROSS}gcc" $CFLAGS $LDFLAGS -o "$p" "$SR/usr/lib/crt1.o" $CRTI "$p.c" sgx2d.c -lm -lc "$LIBGCC" $CRTN
done
# shellcheck disable=SC2086
"${CROSS}gcc" $CFLAGS $LDFLAGS -o sgxinfo "$SR/usr/lib/crt1.o" $CRTI sgxinfo.c -lc "$LIBGCC" $CRTN
# shellcheck disable=SC2086
"${CROSS}gcc" $CFLAGS -fPIC -shared $LDFLAGS -I"$SR/usr/include/SDL2" -D_REENTRANT \
    -o libsgxsdl.so $CRTI sgxsdl.c sgx2d.c -lSDL2 -lm -lc "$LIBGCC" $CRTN
