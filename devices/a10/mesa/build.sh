#!/bin/sh
# build.sh -- Mesa with the G9P compiler (devices/a10/mesa/files) for the
# iPad 7, inside the cascadia-a10-mesa image (Dockerfile) with
# devices/a10 at /a10. Puts files/ into the tree, applies mesa.patch, builds
# into build/mesa/build; the tools land in build/mesa/out.
#   docker run --rm -v "$PWD/devices/a10:/a10" cascadia-a10-mesa sh /a10/mesa/build.sh [driver]
# (driver: the Gallium driver with EGL / GBM / OpenGL ES instead of g9p-cc)
set -eu
B=/a10/build/mesa
S=$B/src
g() { git -c safe.directory='*' -c core.checkStat=minimal -c core.trustctime=false -C "$S" "$@"; }
COMMIT=$(sed -n 's/^COMMIT=\([0-9a-f]*\).*/\1/p' /a10/mesa/fetch.sh)
[ "$(g rev-parse -q --verify HEAD)" = "$COMMIT" ] || { echo "error: build/mesa/src not at $COMMIT (mesa/fetch.sh)" >&2; exit 1; }
# the tree: pristine Mesa + the patch + our files (rsync-like: only changed files touched)
if ! g diff --quiet || [ -n "$(g ls-files --others --exclude-standard | grep -v '^src/g9p/' | head -1)" ]; then
    g diff --name-only | while read -r f; do g checkout -q -- "$f"; done
fi
g apply /a10/mesa/mesa.patch
cp -R /a10/mesa/files/. "$S/"
if [ "${1:-}" = driver ]; then
    # the Gallium driver (g9p) with EGL / GBM / OpenGL ES, for the iPad:
    # prefix /usr/local/g9p, installed into build/mesa/install-drv
    BD=$B/build-drv
    # softpipe: Mesa's software path (EGL's wl_shm on Wayland, GLX's
    # XPutImage on X11), which takes g9p too (GALLIUM_DRIVER=g9p
    # LIBGL_ALWAYS_SOFTWARE=1); GLX for libGL (programs linked with it)
    OPTS="--buildtype=debugoptimized -Dprefix=/usr/local/g9p -Dlibdir=lib
        -Dgallium-drivers=g9p,softpipe -Dvulkan-drivers= -Dplatforms=x11,wayland -Dglx=dri
        -Degl=enabled -Dgbm=enabled -Dopengl=true -Dgles1=disabled -Dgles2=enabled -Dglvnd=disabled
        -Dllvm=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled
        -Dshader-cache=disabled -Dtools= -Dbuild-tests=false -Dgallium-va=disabled
        -Dgallium-rusticl=false -Dvideo-codecs= -Dteflon=false -Dlmsensors=disabled
        -Dmicrosoft-clc=disabled -Dspirv-tools=disabled -Dxmlconfig=disabled"
    if [ ! -f "$BD/build.ninja" ]; then
        # shellcheck disable=SC2086
        meson setup "$BD" "$S" $OPTS
    elif [ "$(cat "$BD/.cascadia-options" 2>/dev/null)" != "$OPTS" ]; then
        # shellcheck disable=SC2086
        meson setup --reconfigure "$BD" "$S" $OPTS
    fi
    printf '%s\n' "$OPTS" > "$BD/.cascadia-options"
    ninja -C "$BD"
    rm -rf "$B/install-drv"
    DESTDIR="$B/install-drv" ninja -C "$BD" install >/dev/null
    I=$B/install-drv/usr/local/g9p
    mkdir -p "$I/bin"
    for t in /a10/tools/gpu/gl/*.c; do
        n=$(basename "$t" .c)
        cc -O2 -Wall -Wno-unused-function -I"$I/include" "$t" -o "$I/bin/$n" -L"$I/lib" -Wl,-rpath-link,"$I/lib" \
            -Wl,-rpath,/usr/local/g9p/lib -lEGL -lGLESv2 -lm
    done
    # the Wayland client (xdg-shell from wayland-protocols)
    W=$B/wl
    mkdir -p "$W"
    X=/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
    wayland-scanner client-header "$X" "$W/xdg-shell-client-protocol.h"
    wayland-scanner private-code "$X" "$W/xdg-shell.c"
    cc -O2 -Wall -I"$I/include" -I"$W" /a10/tools/gpu/gl/wl/g9p-glwl.c "$W/xdg-shell.c" -o "$I/bin/g9p-glwl" \
        -L"$I/lib" -Wl,-rpath-link,"$I/lib" -Wl,-rpath,/usr/local/g9p/lib -lEGL -lGLESv2 -lwayland-client \
        -lwayland-egl -lm
    # the X11 client (desktop GL through GLX, Xwayland)
    cc -O2 -Wall -I"$I/include" /a10/tools/gpu/gl/x11/g9p-glx.c -o "$I/bin/g9p-glx" -L"$I/lib" \
        -Wl,-rpath-link,"$I/lib" -Wl,-rpath,/usr/local/g9p/lib -lGL -lX11 -lm
    install -m 755 /a10/mesa/g9p-run "$I/bin/g9p-run"
    # the stripped tree for the iPad
    rm -rf /tmp/g9p && cp -a "$I" /tmp/g9p && strip /tmp/g9p/lib/*.so.* /tmp/g9p/bin/g9p-gl* 2>/dev/null
    tar -C /tmp -czf "$B/g9p-mesa.tar.gz" g9p
    echo "==> build/mesa/g9p-mesa.tar.gz (mesa/install.sh puts it on the iPad)"
    echo "==> build/mesa/install-drv/usr/local/g9p"
    exit 0
fi
if [ ! -f "$B/build/build.ninja" ]; then
    meson setup "$B/build" "$S" --buildtype=debugoptimized -Dprefix=/usr/local \
        -Dgallium-drivers= -Dvulkan-drivers= -Dplatforms= -Dglx=disabled -Degl=disabled \
        -Dgbm=disabled -Dopengl=true -Dgles1=disabled -Dgles2=enabled -Dllvm=disabled \
        -Dshared-glapi=enabled -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled \
        -Dtools=g9p -Dbuild-tests=false
fi
ninja -C "$B/build" src/g9p/g9p-cc
mkdir -p "$B/out"
cp "$B/build/src/g9p/g9p-cc" "$B/out/"
echo "==> build/mesa/out/g9p-cc"
