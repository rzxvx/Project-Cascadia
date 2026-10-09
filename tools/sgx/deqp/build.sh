#!/bin/sh
# build.sh -- dEQP-GLES2 for the iPad (docs/research/p105-mesa.md, M24):
# VK-GL-CTS's GLES 2 module for the surfaceless EGL platform, built for
# armhf Alpine under ARM emulation, into build/deqp (out of git).
#
#   tools/sgx/deqp/build.sh             (on the Mac: fetch, build)
#   tools/sgx/deqp/build.sh install     (and copy it to root@10.55.0.2:/root/deqp)
#
# On the iPad, then:
#
#   cd /root/deqp && python3 deqp-run.py /root/deqp OUT dEQP-GLES2.functional.clipping ...
#   SGX_WRAP=/root/sgx-dev python3 deqp-run.py ...      (a test build's Mesa)
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
D=$ROOT/build/deqp
TAG=opengl-es-cts-3.2.9.3

mkdir -p "$D"
if [ ! -d "$D/src" ]; then
    git clone --depth 1 --branch $TAG https://github.com/KhronosGroup/VK-GL-CTS.git "$D/src"
    (cd "$D/src" && python3 external/fetch_sources.py)
fi
mkdir -p "$D/stub"
cp "$ROOT/tools/sgx/deqp/stub/execinfo.h" "$D/stub/"
# musl: no execinfo.h (the stub), the POSIX version asked for, glslang's
# uint32_t without <cstdint>; CMake 4 and the old minimum
docker run --rm --init --platform linux/arm/v6 -v "$D:/deqp" alpine:3.24 sh -c '
    set -e
    apk add --no-cache cmake ninja g++ python3 libpng-dev zlib-dev mesa-dev linux-headers >/dev/null
    cd /deqp
    F="-O2 -I/deqp/stub -D_XOPEN_SOURCE=600 -D_DEFAULT_SOURCE"
    cmake -G Ninja -S src -B out -DDEQP_TARGET=surfaceless -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_C_FLAGS="$F" -DCMAKE_CXX_FLAGS="$F -include cstdint" \
          -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -Wno-dev >/dev/null
    ninja -C out deqp-gles2'
echo "built: $D/out/modules/gles2/deqp-gles2"

if [ "$1" = install ]; then
    G=$D/out/modules/gles2
    ssh root@10.55.0.2 mkdir -p /root/deqp
    tar cf - -C "$G" deqp-gles2 gles2 | ssh root@10.55.0.2 'tar xf - -C /root/deqp'
    scp -q "$ROOT/tools/sgx/deqp-run.py" root@10.55.0.2:/root/deqp/
    # the case list (the surface's arguments, or it comes out empty)
    ssh root@10.55.0.2 'cd /root/deqp && EGL_PLATFORM=surfaceless sgx-gl ./deqp-gles2 \
        --deqp-runmode=stdout-caselist --deqp-surface-type=pbuffer \
        --deqp-gl-config-name=rgba8888d24s8ms0 --deqp-surface-width=256 \
        --deqp-surface-height=256 | sed -n "s/^TEST: //p" > cases.txt && wc -l cases.txt'
fi
