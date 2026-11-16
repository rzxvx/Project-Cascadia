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
    mkdir -p "$OUT/src"
    rm -f "$OUT"/*.apk
    cat > "$OUT/src/APKBUILD" <<APK
# Maintainer: Project Cascadia
pkgname=cascadia-gl
pkgver=$MESA_VER
pkgrel=0
pkgdesc="Stands in for Alpine's Mesa: Project Cascadia's GPU Mesa is in /usr/local/lib/sgx-mesa"
url="https://github.com/rzxvx/Project-Cascadia"
arch="noarch"
license="MIT"
options="!check"
provides="
	mesa-egl=$MESA_VER-r0
	mesa-gbm=$MESA_VER-r0
	mesa-gl=$MESA_VER-r0
	mesa-gles=$MESA_VER-r0
	so:libEGL.so.1=1.0.0
	so:libGL.so.1=1.2.0
	so:libGLESv1_CM.so.1=1.1.0
	so:libGLESv2.so.2=2.0.0
	so:libgbm.so.1=1.0.0
	"
package() {
	mkdir -p "\$pkgdir"
}
APK
    docker run --rm -v "$OUT":/out "alpine:$ALPINE" sh -ec '
        apk add -q alpine-sdk >/dev/null
        cp -r /out/src /root/cascadia-gl
        cd /root/cascadia-gl
        abuild-keygen -a -n >/root/build.log 2>&1
        abuild -F -r >>/root/build.log 2>&1 || { tail -30 /root/build.log; exit 1; }
        cp /root/packages/*/*/cascadia-gl-*.apk /out/'
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
