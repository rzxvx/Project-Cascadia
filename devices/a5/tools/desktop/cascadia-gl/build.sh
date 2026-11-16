#!/bin/sh
# cascadia-gl: an empty apk that stands in for Alpine's Mesa.
#
#   sh tools/desktop/cascadia-gl/build.sh [install]
#
# Our Mesa (./cascadia a5 dev mesa: the GPU's driver, /usr/local/lib/sgx-mesa,
# first on /etc/ld-musl-armhf.path) is what every program loads.  Alpine's own
# mesa, mesa-egl/-gbm/-gl/-gles, came along only because packages depend on
# them by name or by soname (xorg-server: mesa-egl, libGL, libgbm; supertux and
# glew: libGL) -- and with them llvm22-libs, spirv-tools and glslang, ~210 MB
# that nothing ran.  This package provides those names, so apk is satisfied
# and installs none of it.  Installed before XFCE (tools/desktop/make-root.sh).
set -eu
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
ALPINE="${ALPINE:-3.24}"
DEVICE="${DEVICE:-root@10.55.0.2}"
OUT="$ROOT/build/cascadia-gl"
# the Mesa our driver is built from (mesa/: libgallium-26.1.8)
MESA_VER=26.1.8

build() {
    # apk-tools 3's own mkpkg: an empty package with only metadata (abuild
    # refused it); the root is armhf, the package noarch
    rm -rf "$OUT"; mkdir -p "$OUT/root"
    V=$MESA_VER-r0
    docker run --rm -v "$OUT":/w "alpine:$ALPINE" apk mkpkg \
        -I name:cascadia-gl -I version:$V -I arch:noarch -I license:MIT \
        -I "description:Stands in for Alpine Mesa: Project Cascadia GPU Mesa is in /usr/local/lib/sgx-mesa" \
        -I "provides:mesa-egl=$V mesa-gbm=$V mesa-gl=$V mesa-gles=$V so:libEGL.so.1=1.0.0 so:libGL.so.1=1.2.0 so:libGLESv1_CM.so.1=1.1.0 so:libGLESv2.so.2=2.0.0 so:libgbm.so.1=1.0.0" \
        -F /w/root -o /w/cascadia-gl-$V.apk
}

if [ "${1:-}" != install ] || ! ls "$OUT"/cascadia-gl-*.apk >/dev/null 2>&1; then
    build
fi
ls -l "$OUT"/cascadia-gl-*.apk
if [ "${1:-}" = install ]; then
    P=$(ls "$OUT"/cascadia-gl-*.apk | head -1)
    ssh "$DEVICE" "cat > /tmp/$(basename "$P")" < "$P"
    ssh "$DEVICE" "apk add --allow-untrusted --no-network /tmp/$(basename "$P") && rm -f /tmp/$(basename "$P")"
fi
