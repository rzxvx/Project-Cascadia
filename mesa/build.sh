#!/bin/sh
# mesa/build.sh -- Mesa with the SGX543MP2's driver, for the iPad.
#
# Runs inside the cascadia-mesa image (mesa/Dockerfile), which ./cascadia
# mesa builds and starts with this repository at /cascadia.  The result is
# build/mesa/install/usr/local/lib/sgx-mesa: libEGL, libGLESv2, libgbm and
# libgallium with only the sgx driver in it, glclear, sgx-gl.
#
# Mesa itself comes from freedesktop.org at the pinned tag (checked by
# commit); this repository has the driver (mesa/files, copied into the
# tree) and the few lines that register it (mesa/mesa.patch).  The first
# build takes long under ARM emulation; after that only what changed is
# built again.
set -eu
ROOT=/cascadia
B=$ROOT/build/mesa
TAG=mesa-26.1.8
COMMIT=0fadfea4f394211946f308458f614839ef253ee8
PREFIX=/usr/local/lib/sgx-mesa

say() { printf '==> %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }
g() { git -c safe.directory='*' -C "$B/src" "$@"; }

mkdir -p "$B"
if [ ! -d "$B/src/.git" ]; then
    say "Mesa $TAG from gitlab.freedesktop.org"
    rm -rf "$B/src"
    git clone -q --depth 1 --branch "$TAG" https://gitlab.freedesktop.org/mesa/mesa.git "$B/src"
fi
[ "$(g rev-parse HEAD)" = "$COMMIT" ] || fail "$B/src is not $TAG ($COMMIT)"

say "the sgx driver into the tree"
# copy only what changed, so ninja rebuilds only that
( cd "$ROOT/mesa/files" && find . -type f ) | while read -r f; do
    if ! cmp -s "$ROOT/mesa/files/$f" "$B/src/$f"; then
        mkdir -p "$(dirname "$B/src/$f")"
        cp "$ROOT/mesa/files/$f" "$B/src/$f"
        printf '    %s\n' "${f#./}"
    fi
done
# the glue: applied once; an older version of it is taken out first
if ! g apply --reverse --check "$ROOT/mesa/mesa.patch" 2>/dev/null; then
    changed=$(g diff --name-only)
    [ -z "$changed" ] || g checkout -q -- $changed
    g apply "$ROOT/mesa/mesa.patch"
    printf '    applied mesa/mesa.patch\n'
fi

if [ ! -f "$B/build/build.ninja" ]; then
    say "configuring"
    meson setup "$B/build" "$B/src" --prefix="$PREFIX" --libdir=lib --buildtype=debugoptimized \
        -Dgallium-drivers=sgx -Dvulkan-drivers= -Dplatforms= \
        -Dglx=disabled -Degl=enabled -Dgbm=enabled -Dgles1=disabled -Dgles2=enabled \
        -Dopengl=true -Dglvnd=disabled -Dllvm=disabled -Dvalgrind=disabled \
        -Dlibunwind=disabled -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled \
        -Dshader-cache=disabled -Dtools= -Dbuild-tests=false -Dgallium-va=disabled \
        -Dgallium-rusticl=false -Dvideo-codecs= -Dteflon=false -Dlmsensors=disabled \
        -Dmicrosoft-clc=disabled -Dspirv-tools=disabled >/dev/null
fi

say "building (the first time is slow under emulation)"
ninja -C "$B/build"
rm -rf "$B/install"
DESTDIR="$B/install" ninja -C "$B/build" install >/dev/null

I=$B/install$PREFIX
mkdir -p "$I/bin"
cc -O2 -Wall -I"$I/include" "$ROOT/tools/sgx/gl/glclear.c" -L"$I/lib" -lEGL -lGLESv2 \
    -Wl,-rpath-link,"$I/lib" -Wl,-rpath,"$PREFIX/lib" -o "$I/bin/glclear"
cp "$ROOT/mesa/sgx-gl" "$I/bin/sgx-gl"
say "built: build/mesa/install$PREFIX"
