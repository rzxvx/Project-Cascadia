#!/usr/bin/env bash
# One pinned G11 Mesa build shared by XS Max and iPad; no rootfs replacement.
set -euo pipefail
A12="$(cd "$(dirname "$0")/.." && pwd)"
HERE="$A12/mesa"
SRC="${A12_MESA_SOURCE:-$A12/build/mesa}"
STAGE="${A12_MESA_STAGE:-$A12/build/mesa-stage}"
REV=24a0a3bddf5588d7f087e8f40658ba5e17af6094
PATCH_SHA=1e3936b8ef4ea523c4c7c6d4f6823d5c576f4fe876c783e4f0bf55c844a96403
JOBS="${MESA_JOBS:-2}"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo 'MESA_JOBS must be a positive integer' >&2; exit 2; }
[ "$(uname -m)" = aarch64 ] || { echo 'Native Linux arm64 Mesa build required' >&2; exit 2; }
printf '%s  %s\n' "$PATCH_SHA" "$HERE/mesa-0001-asahi-g11.patch" | sha256sum -c -
for t in git meson ninja pkg-config rustc bindgen llvm-config; do
    command -v "$t" >/dev/null || { echo "missing build dependency: $t" >&2; exit 1; }
done
# Do not reset/clean an arbitrary source tree. Existing patched trees are reusable.
if [ ! -e "$SRC" ]; then
    mkdir -p "$SRC"
    git -C "$SRC" init -q
    git -C "$SRC" -c gc.auto=0 -c pack.threads=1 -c pack.windowMemory=32m fetch --depth=1 --no-tags https://gitlab.freedesktop.org/mesa/mesa.git "$REV"
    git -C "$SRC" checkout -q --detach FETCH_HEAD
fi
[ "$(git -C "$SRC" rev-parse HEAD)" = "$REV" ] || { echo 'Wrong Mesa revision; source left untouched' >&2; exit 1; }
if git -C "$SRC" apply --check "$HERE/mesa-0001-asahi-g11.patch" 2>/dev/null; then
    git -C "$SRC" apply "$HERE/mesa-0001-asahi-g11.patch"
else
    git -C "$SRC" apply --reverse --check "$HERE/mesa-0001-asahi-g11.patch"
fi
# Default matches the proven Wayland/EGL GLES runtime. X11/GLX is an explicit
# additional build, not a claim that XFCE acceleration has been device-tested.
X11="${A12_MESA_X11:-0}"
case "$X11" in
  0) PLATFORMS=wayland; GLX=disabled ;;
  1) PLATFORMS=x11,wayland; GLX=dri ;;
  *) echo 'A12_MESA_X11 must be 0 or 1' >&2; exit 2 ;;
esac

HOST="$SRC/build-cascadia-host"
TARGET="$SRC/build-cascadia-runtime-${A12_MESA_X11:-0}"
if [ ! -f "$HOST/build.ninja" ]; then
    meson setup "$HOST" "$SRC" -Dbuildtype=release -Dgallium-drivers= -Dvulkan-drivers= \
        -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false \
        -Dgles1=disabled -Dgles2=disabled -Dtools=asahi -Dmesa-clc=enabled \
        -Dprecomp-compiler=enabled -Dllvm=enabled
fi
ninja -C "$HOST" -j"$JOBS" src/compiler/clc/mesa_clc src/asahi/clc/asahi_clc src/compiler/spirv/vtn_bindgen2
export PATH="$HOST/src/compiler/clc:$HOST/src/asahi/clc:$HOST/src/compiler/spirv:$PATH"
# Rusticl/Vulkan are not yet validated on the production iPad driver. LLVM is
# enabled for parity with the passing runtime; installer checks target ABI.
if [ ! -f "$TARGET/build.ninja" ]; then
    meson setup "$TARGET" "$SRC" -Dbuildtype=release -Dgallium-drivers=asahi \
        -Dvulkan-drivers= -Dgallium-rusticl=false -Dllvm=enabled -Dshared-llvm=enabled \
        -Dplatforms="$PLATFORMS" -Dglx="$GLX" -Degl=enabled -Dgbm=enabled \
        -Dgles1=disabled -Dgles2=enabled -Dopengl=true -Dmesa-clc=system \
        -Dprecomp-compiler=system -Dmesa-clc-bundle-headers=enabled -Dstatic-libclc=all \
        -Dprefix=/usr -Dlibdir=lib
fi
ninja -C "$TARGET" -j"$JOBS"
mkdir -p "$STAGE"
if [ -f "$STAGE/.cascadia-features" ] && [ "$(cat "$STAGE/.cascadia-features")" != "$X11" ]; then
    echo 'Mesa stage contains another feature set; choose a separate A12_MESA_STAGE' >&2
    exit 1
fi
printf '%s\n' "$X11" > "$STAGE/.cascadia-features"
DESTDIR="$STAGE" meson install -C "$TARGET" --no-rebuild --quiet --strip
python3 "$HERE/runtime.py" manifest "$STAGE" --revision "$REV" --patch-sha "$PATCH_SHA"
echo "A12 Mesa staged: $STAGE (not installed on host)"
