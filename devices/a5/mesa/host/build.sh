#!/usr/bin/env bash
#
# mesa/host/build.sh [arm] -- the driver on a Linux PC, against Mesa's
# drm-shim instead of the render node: no iPad needed. Renders do nothing
# there, but everything up to the kick runs: the draw module and the
# vertices it hands the frame (SGX_DEBUG_DRAW=1), every word of a clear or
# a draw (SGX_DEBUG=frame, with the fake pack this makes).
#
#   bash mesa/host/build.sh          for the host itself (x86-64), debug
#   bash mesa/host/build.sh arm      armhf, cross-compiled, to run under
#                                    qemu-arm-static (32-bit ARM as on the
#                                    iPad; glibc, not the device's musl)
#   mesa/host/run gltri              then a program against it
#   mesa/host/run --arm gltri
#
# Mesa's source is build/mesa/src (mesa/fetch.sh fetches it when missing),
# or MESA_SRC=DIR, a checkout of the pinned commit; the driver and the glue
# go in as mesa/build.sh puts them, and mesa/host/drm-shim-t64.patch with
# them (drm-shim's wrappers for armhf glibc's 64-bit time_t calls). The
# result lands in build/mesa-host/{host,arm}, the fake pack in
# build/mesa-host/fakepack.
#
# Needs gcc, ninja, bison, flex, glslang-tools, libdrm-dev, zlib1g-dev,
# libelf-dev, meson >= 1.4 and python3's mako and pyyaml (pip install meson
# mako pyyaml); for arm also qemu-user-static, gcc-arm-linux-gnueabihf,
# g++-arm-linux-gnueabihf and libdrm-dev:armhf zlib1g-dev:armhf (after
# dpkg --add-architecture armhf).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$ROOT/mesa/host"
ARCH="${1:-host}"
SRC="${MESA_SRC:-$ROOT/build/mesa/src}"
O="$ROOT/build/mesa-host/$ARCH"
B="$O/build"
COMMIT=$(sed -n 's/^COMMIT=\([0-9a-f]*\).*/\1/p' "$ROOT/mesa/fetch.sh")

say() { printf '==> %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }
g() { git -c safe.directory='*' -C "$SRC" "$@"; }

case "$ARCH" in host|arm) ;; *) fail "usage: mesa/host/build.sh [arm]" ;; esac
[ -n "${MESA_SRC:-}" ] || [ -d "$SRC" ] || bash "$ROOT/mesa/fetch.sh"
[ "$(g rev-parse -q --verify HEAD 2>/dev/null)" = "$COMMIT" ] \
    || fail "$SRC is not Mesa at $COMMIT (mesa/fetch.sh)"

say "the sgx driver into $SRC"
( cd "$ROOT/mesa/files" && find . -type f ) | while read -r f; do
    if ! cmp -s "$ROOT/mesa/files/$f" "$SRC/$f"; then
        mkdir -p "$(dirname "$SRC/$f")"
        cp "$ROOT/mesa/files/$f" "$SRC/$f"
        printf '    %s\n' "${f#./}"
    fi
done
if ! g apply --reverse --check "$ROOT/mesa/mesa.patch" 2>/dev/null; then
    changed=$(g diff --name-only)
    [ -z "$changed" ] || g checkout -q -- $changed
    rm -f "$SRC/src/drm-shim/t64.c"
    g apply "$ROOT/mesa/mesa.patch"
    printf '    applied mesa/mesa.patch\n'
fi
if ! g apply --reverse --check "$HERE/drm-shim-t64.patch" 2>/dev/null; then
    g apply "$HERE/drm-shim-t64.patch"
    printf '    applied mesa/host/drm-shim-t64.patch\n'
fi

OPTS="-Dgallium-drivers=sgx -Dvulkan-drivers= -Dplatforms= -Dglx=disabled
    -Degl=enabled -Dgbm=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true
    -Dglvnd=disabled -Dllvm=disabled -Dvalgrind=disabled -Dlibunwind=disabled
    -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled
    -Dtools=drm-shim -Dbuild-tests=false -Dgallium-va=disabled
    -Dgallium-rusticl=false -Dvideo-codecs= -Dteflon=false -Dlmsensors=disabled
    -Dmicrosoft-clc=disabled -Dspirv-tools=disabled"
if [ "$ARCH" = arm ]; then
    # -mtls-dialect=gnu as in mesa/build.sh: with gnu2 the current GL
    # context comes back wrong on 32-bit ARM (a crash in _mesa_make_current)
    OPTS="$OPTS --buildtype=debugoptimized --cross-file=$HERE/arm.cross
    -Dc_args=-mtls-dialect=gnu -Dcpp_args=-mtls-dialect=gnu"
    CC=arm-linux-gnueabihf-gcc
else
    OPTS="$OPTS --buildtype=debug"
    CC=${CC:-cc}
fi
mkdir -p "$O"
if [ ! -f "$B/build.ninja" ]; then
    say "configuring ($ARCH)"
    # shellcheck disable=SC2086
    meson setup "$B" "$SRC" $OPTS >/dev/null
    printf '%s\n' "$OPTS" > "$B/.cascadia-options"
elif [ "$(cat "$B/.cascadia-options" 2>/dev/null)" != "$OPTS" ]; then
    say "configuring again (the options changed)"
    # shellcheck disable=SC2086
    meson setup --reconfigure "$B" "$SRC" $OPTS >/dev/null
    printf '%s\n' "$OPTS" > "$B/.cascadia-options"
fi

say "building ($ARCH)"
ninja -C "$B"

say "glclear and gltri"
for p in glclear gltri; do
    $CC -O1 -g -Wall -I"$SRC/include" "$ROOT/tools/sgx/gl/$p.c" \
        -L"$B/src/egl" -L"$B/src/mesa/glapi/es2api" -lEGL -lGLESv2 -lm \
        -Wl,-rpath-link,"$B/src/gallium/targets/dri:$B/src/gbm" -o "$O/$p"
done

python3 "$HERE/fakepack.py" "$ROOT/build/mesa-host/fakepack" >/dev/null
if [ "$ARCH" = arm ]; then say "done: mesa/host/run --arm gltri"; else say "done: mesa/host/run gltri"; fi
