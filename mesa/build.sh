#!/bin/sh
# mesa/build.sh -- Mesa with the SGX543MP2's driver, for the iPad.
#
# Runs inside the cascadia-mesa image (mesa/Dockerfile), which ./cascadia
# mesa builds and starts with this repository at /cascadia.  The result is
# build/mesa/install/usr/local/lib/sgx-mesa: libEGL, libGLESv2, libgbm and
# libgallium with only the sgx driver in it, glclear, gltri, sgx-gl.
#
# Mesa itself is fetched on the host beforehand (mesa/fetch.sh, which
# ./cascadia mesa runs first: git under emulation is far too slow); this
# repository has the driver (mesa/files, copied into the tree) and the few
# lines that register it (mesa/mesa.patch).  The first build takes long
# under ARM emulation; after that only what changed is built again.
set -eu
ROOT=/cascadia
B=$ROOT/build/mesa
COMMIT=$(sed -n 's/^COMMIT=\([0-9a-f]*\).*/\1/p' "$ROOT/mesa/fetch.sh")
PREFIX=/usr/local/lib/sgx-mesa

say() { printf '==> %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }
# the tree was checked out by the host's git: its index has the host's
# inode numbers, so compare files by size and time only, or git would read
# every file again (slow, under emulation)
g() { git -c safe.directory='*' -c core.checkStat=minimal -c core.trustctime=false -C "$B/src" "$@"; }

[ "$(g rev-parse -q --verify HEAD 2>/dev/null)" = "$COMMIT" ] \
    || fail "build/mesa/src is not Mesa at $COMMIT -- ./cascadia mesa fetches it (mesa/fetch.sh)"

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

# -mtls-dialect=gnu: left to itself, Mesa's meson.build finds that the
# compiler takes -mtls-dialect=gnu2 (TLS descriptors) and builds with it,
# and on 32-bit ARM the current GL context then comes back wrong: garbage on
# the iPad (glGetString NULL, "Inside glBegin/glEnd", a bus error), a crash
# in _mesa_make_current under qemu with glibc.  The classic dialect is right
# on both.  The options are kept in a stamp, so a change reconfigures.
# MESA_BUILD=release: no assertions, optimised (build/mesa/build-release,
# install-release) -- for measuring speed (M25); the default keeps them
if [ "${MESA_BUILD:-}" = release ]; then
    BT="--buildtype=release -Db_ndebug=true"; BD=$B/build-release; ID=$B/install-release
else
    BT=--buildtype=debugoptimized; BD=$B/build; ID=$B/install
fi
OPTS="--prefix=$PREFIX --libdir=lib $BT
    -Dc_args=-mtls-dialect=gnu -Dcpp_args=-mtls-dialect=gnu
    -Dgallium-drivers=sgx -Dvulkan-drivers= -Dplatforms=wayland
    -Dglx=disabled -Degl=enabled -Dgbm=enabled -Dgles1=disabled -Dgles2=enabled
    -Dopengl=true -Dglvnd=disabled -Dllvm=disabled -Dvalgrind=disabled
    -Dlibunwind=disabled -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled
    -Dshader-cache=disabled -Dtools= -Dbuild-tests=false -Dgallium-va=disabled
    -Dgallium-rusticl=false -Dvideo-codecs= -Dteflon=false -Dlmsensors=disabled
    -Dmicrosoft-clc=disabled -Dspirv-tools=disabled"
if [ ! -f "$BD/build.ninja" ]; then
    say "configuring"
    # shellcheck disable=SC2086
    meson setup "$BD" "$B/src" $OPTS >/dev/null
    printf '%s\n' "$OPTS" > "$BD/.cascadia-options"
elif [ "$(cat "$BD/.cascadia-options" 2>/dev/null)" != "$OPTS" ]; then
    say "configuring again (the options changed: everything is built again)"
    # shellcheck disable=SC2086
    meson setup --reconfigure "$BD" "$B/src" $OPTS >/dev/null
    printf '%s\n' "$OPTS" > "$BD/.cascadia-options"
fi

say "building (the first time is slow under emulation)"
ninja -C "$BD"
rm -rf "$ID"
DESTDIR="$ID" ninja -C "$BD" install >/dev/null

I=$ID$PREFIX
mkdir -p "$I/bin"
for t in glclear gltri glfs gltex glblend gldepth glspeed glpersp glsize glseam glcull gldiscard glstencil glchurn glwrap; do
    cc -O2 -Wall -I"$I/include" "$ROOT/tools/sgx/gl/$t.c" -L"$I/lib" -lEGL -lGLESv2 -lm \
        -Wl,-rpath-link,"$I/lib" -Wl,-rpath,"$PREFIX/lib" -o "$I/bin/$t"
done
# on the screen: GBM and KMS besides
cc -O2 -Wall -I"$I/include" $(pkg-config --cflags libdrm) "$ROOT/tools/sgx/gl/glkms.c" \
    -L"$I/lib" -lEGL -lGLESv2 -lgbm $(pkg-config --libs libdrm) -lm \
    -Wl,-rpath-link,"$I/lib" -Wl,-rpath,"$PREFIX/lib" -o "$I/bin/glkms"
cp "$ROOT/mesa/sgx-gl" "$ROOT/mesa/frame-bisect" "$I/bin/"
say "built: ${ID#$ROOT/}$PREFIX"
