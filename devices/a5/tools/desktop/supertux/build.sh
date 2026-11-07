#!/bin/sh
# tools/desktop/supertux/build.sh [install] -- SuperTux 0.7.0 as an Alpine
# package for the iPad (APKBUILD here; docs/research/p105-mesa.md, M42),
# built by abuild in docker for armhf (linux/arm/v6, the iPad's userland)
# into build/supertux/ -- C++ under emulation, the better part of an hour.
# The 292 MB source stays in build/supertux/distfiles between builds.
# "install": supertux and supertux-data onto the iPad (DEVICE=root@10.55.0.2),
# those in build/supertux/ (built first if there are none).
set -eu
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
ALPINE="${ALPINE:-3.24}"
# Jobs by the memory docker has, about a GB each, at most 8 and the cores:
# under ARM emulation every compiler takes several times its native size,
# and a 4 GB machine ran out with the old fixed count.  SUPERTUX_JOBS= overrides.
SUPERTUX_JOBS="${SUPERTUX_JOBS:-$(docker info --format '{{.MemTotal}} {{.NCPU}}' 2>/dev/null |
    awk '{ j = int($1 / 1073741824); if (j > $2) j = $2; if (j > 8) j = 8; if (j < 1) j = 1; print j }')}"
DEVICE="${DEVICE:-root@10.55.0.2}"
OUT="$ROOT/build/supertux"
build() {
    mkdir -p "$OUT/src" "$OUT/distfiles"
    rm -rf "$OUT/src" && mkdir "$OUT/src"
    cp "$ROOT"/tools/desktop/supertux/APKBUILD "$ROOT"/tools/desktop/supertux/*.patch "$OUT/src/"
    rm -f "$OUT"/*.apk
    # As root (abuild -F), not a sudo user: under qemu registered without the
    # C flag (Arch's qemu-user-static-binfmt) setuid does nothing, and sudo
    # in here failed with "effective uid is not 0" before the build began.
    # The key goes in by hand: abuild-keygen -i calls doas, even as root.
    docker run --rm --platform linux/arm/v6 -e SUPERTUX_JOBS="$SUPERTUX_JOBS" -v "$OUT":/out -v "$OUT/distfiles":/var/cache/distfiles \
        "alpine:$ALPINE" sh -ec '
        apk add -q alpine-sdk >/dev/null
        chmod 777 /var/cache/distfiles
        cp -r /out/src /root/supertux
        ( cd /root/supertux && export SUPERTUX_JOBS=${SUPERTUX_JOBS:-6} &&
                       abuild-keygen -a -n >/root/build.log 2>&1 && cp /root/.abuild/*.rsa.pub /etc/apk/keys/ &&
                   abuild -F checksum >>/root/build.log 2>&1 && abuild -F -r >>/root/build.log 2>&1 ) ||
            { tail -40 /root/build.log; cp /root/build.log /out/; exit 1; }
        cp /root/packages/*/*/supertux-*.apk /root/build.log /out/'
}
if [ "${1:-}" != install ] || ! ls "$OUT"/supertux-[0-9]*.apk >/dev/null 2>&1; then
    build
fi
ls -l "$OUT"/*.apk
if [ "${1:-}" = install ]; then
    # Its libraries first, offline, from docker: every depend of the package
    # (so:libGLEW, SDL2, OpenAL, ... and libraqm, which 0.6.3 did not need).
    # Not only libraqm: a root without SuperTux's old 0.6.3 has none of
    # them, and --force-broken-world below would then skip supertux itself
    # in silence and leave a world every later apk add trips over.
    DEPS=$(for P in "$OUT"/supertux-[0-9]*.apk; do tar -xzOf "$P" .PKGINFO 2>/dev/null; done |
           sed -n 's/^depend = //p' | grep -v -e '^supertux' -e '^so:libc\.musl' | sort -u | tr '\n' ' ')
    [ -n "$DEPS" ] || { echo "no depends in $OUT/supertux-*.apk's .PKGINFO" >&2; exit 1; }
    DEVICE="$DEVICE" sh "$ROOT/tools/device-apk.sh" $DEPS
    # (in /var/tmp, the NFS root: the data are 283 MB, /tmp is 128 MB of RAM)
    for P in "$OUT"/supertux-data-*.apk "$OUT"/supertux-[0-9]*.apk; do
        ssh "$DEVICE" "cat > /var/tmp/$(basename "$P")" < "$P"
    done
    ssh "$DEVICE" "cd /var/tmp && apk add --allow-untrusted --no-network --force-broken-world ./supertux-data-*.apk ./supertux-[0-9]*.apk && rm -f ./supertux-*.apk"
fi
