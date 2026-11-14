#!/bin/sh
# tools/desktop/ppsspp/build.sh [install] -- PPSSPP (the PSP emulator, its
# SDL frontend) as an Alpine package for the iPad 2 (APKBUILD here: Alpine has
# none), built for the A5 by abuild in docker for armhf (linux/arm/v6, the
# iPad's userland) into build/ppsspp/.  C++ under emulation: hours the first
# time; build/ppsspp/ccache keeps every object, so a build again after a
# change takes minutes, and build/ppsspp/distfiles the 60 MB source.
# "install": PPSSPP onto the iPad (DEVICE=root@10.55.0.2), the package in
# build/ppsspp/ (built first if there is none).
set -eu
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
ALPINE="${ALPINE:-3.24}"
# Jobs by the memory docker has, about a GB each, at most 8 and the cores
# (C++ under ARM emulation takes several times its native size).
PPSSPP_JOBS="${PPSSPP_JOBS:-$(docker info --format '{{.MemTotal}} {{.NCPU}}' 2>/dev/null |
    awk '{ j = int($1 / 1073741824); if (j > $2) j = $2; if (j > 8) j = 8; if (j < 1) j = 1; print j }')}"
DEVICE="${DEVICE:-root@10.55.0.2}"
OUT="$ROOT/build/ppsspp"
build() {
    mkdir -p "$OUT/distfiles" "$OUT/ccache"
    rm -rf "$OUT/src" && mkdir "$OUT/src"
    cp "$ROOT"/tools/desktop/ppsspp/APKBUILD "$ROOT"/tools/desktop/ppsspp/*.patch "$OUT/src/"
    rm -f "$OUT"/*.apk
    # As root (abuild -F): under qemu registered without the C flag setuid
    # does nothing.  The key goes in by hand: abuild-keygen -i calls doas.
    # ccache with the base dir: objects are found again whatever the path.
    docker run --rm --platform linux/arm/v6 -e PPSSPP_JOBS="$PPSSPP_JOBS" -e USE_CCACHE=1 \
        -e CCACHE_DIR=/ccache -e CCACHE_BASEDIR=/root/ppsspp -e CCACHE_MAXSIZE=4G \
        -v "$OUT":/out -v "$OUT/distfiles":/var/cache/distfiles -v "$OUT/ccache":/ccache \
        "alpine:$ALPINE" sh -ec '
        apk add -q alpine-sdk >/dev/null
        chmod 777 /var/cache/distfiles /ccache
        cp -r /out/src /root/ppsspp
        ( cd /root/ppsspp && abuild-keygen -a -n >/root/build.log 2>&1 && cp /root/.abuild/*.rsa.pub /etc/apk/keys/ &&
                       abuild -F checksum >>/root/build.log 2>&1 && abuild -F -r >>/root/build.log 2>&1 ) ||
            { tail -60 /root/build.log; cp /root/build.log /out/; exit 1; }
        cp /root/packages/*/*/ppsspp-*.apk /root/build.log /out/
        cp /root/ppsspp/APKBUILD /out/APKBUILD.built'
}
if [ "${1:-}" != install ] || ! ls "$OUT"/ppsspp-[0-9]*.apk >/dev/null 2>&1; then
    build
fi
ls -l "$OUT"/*.apk
if [ "${1:-}" = install ]; then
    P=$(ls "$OUT"/ppsspp-[0-9]*.apk | head -1)
    ssh "$DEVICE" "cat > /var/tmp/$(basename "$P")" < "$P"
    ssh "$DEVICE" "apk add -q --allow-untrusted /var/tmp/$(basename "$P") && rm -f /var/tmp/$(basename "$P")"
fi
