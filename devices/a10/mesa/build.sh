#!/bin/sh
# build.sh -- Mesa with the G9P compiler (devices/a10/mesa/files) for the
# iPad 7, inside the cascadia-a10-mesa image (Dockerfile) with
# devices/a10 at /a10. Puts files/ into the tree, applies mesa.patch, builds
# into build/mesa/build; the tools land in build/mesa/out.
#   docker run --rm -v "$PWD/devices/a10:/a10" cascadia-a10-mesa sh /a10/mesa/build.sh
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
