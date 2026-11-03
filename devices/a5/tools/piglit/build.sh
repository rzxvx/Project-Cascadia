#!/bin/sh
# tools/piglit/build.sh -- piglit, the OpenGL test suite, for the iPad
# (docs/research/p105-mesa.md, M35): the desktop GL and GLES 2 tests, run
# through waffle (surfaceless EGL, or GBM / X11 / Wayland).
#
# On the host (the build takes about an hour under emulation):
#   git clone --depth 1 https://gitlab.freedesktop.org/mesa/piglit.git build/piglit-src
#   docker build --platform linux/arm/v7 -t cascadia-piglit tools/piglit
#   docker run --rm --init --platform linux/arm/v7 -v $PWD:/cascadia -w /cascadia \
#       cascadia-piglit sh tools/piglit/build.sh
#   tools/device-apk.sh waffle py3-mako py3-markupsafe     (once: piglit's own needs)
#   tools/piglit/install.sh                  /root/piglit, run.sh, pigsum.py
#
# On the iPad (the GL profile less GL 3 and 4, about 40 minutes):
#   /root/piglit/run.sh NAME                 the system's Mesa
#   WRAP=/root/sgx-dev SGX_MESA=$(readlink -f /root/sgx-mesa-rel) /root/piglit/run.sh NAME
#   python3 /root/pigsum.py /root/piglit-results/NAME    what did not pass
# One test alone: LD_LIBRARY_PATH=/root/piglit/lib/piglit/lib PIGLIT_PLATFORM=gbm
#   PIGLIT_SOURCE_DIR=/root/piglit/lib/piglit /root/piglit/lib/piglit/bin/TEST -auto
set -eu
ROOT=/cascadia
SRC=$ROOT/build/piglit-src
B=$ROOT/build/piglit-build
I=$ROOT/build/piglit-install
[ -f "$SRC/CMakeLists.txt" ] || { echo "no piglit source in build/piglit-src" >&2; exit 1; }
if [ ! -f "$B/build.ninja" ]; then
    cmake -S "$SRC" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/ \
        -DPIGLIT_BUILD_GL_TESTS=ON -DPIGLIT_BUILD_GLES1_TESTS=OFF \
        -DPIGLIT_BUILD_GLES2_TESTS=ON -DPIGLIT_BUILD_GLES3_TESTS=OFF \
        -DPIGLIT_BUILD_CL_TESTS=OFF -DPIGLIT_BUILD_VK_TESTS=OFF \
        -DPIGLIT_BUILD_DMA_BUF_TESTS=OFF -DPIGLIT_USE_WAFFLE=ON
fi
ninja -C "$B"
rm -rf "$I"
DESTDIR="$I" ninja -C "$B" install >/dev/null
echo "built: build/piglit-install"
