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
DEVICE="${DEVICE:-root@10.55.0.2}"
OUT="$ROOT/build/supertux"
build() {
    mkdir -p "$OUT/src" "$OUT/distfiles"
    rm -rf "$OUT/src" && mkdir "$OUT/src"
    cp "$ROOT"/tools/desktop/supertux/APKBUILD "$ROOT"/tools/desktop/supertux/*.patch "$OUT/src/"
    rm -f "$OUT"/*.apk
    docker run --rm --platform linux/arm/v6 -v "$OUT":/out -v "$OUT/distfiles":/var/cache/distfiles \
        "alpine:$ALPINE" sh -ec '
        apk add -q alpine-sdk sudo >/dev/null
        adduser -D builder && addgroup builder abuild
        echo "builder ALL=(ALL) NOPASSWD: ALL" > /etc/sudoers.d/builder
        chmod 777 /var/cache/distfiles
        cp -r /out/src /home/builder/supertux && chown -R builder /home/builder/supertux
        su builder -c "cd ~/supertux && export SUPERTUX_JOBS=${SUPERTUX_JOBS:-6} &&
                       abuild-keygen -a -i -n >/dev/null 2>&1 && abuild checksum >/dev/null &&
                       abuild -r >/home/builder/build.log 2>&1" ||
            { tail -40 /home/builder/build.log; cp /home/builder/build.log /out/; exit 1; }
        cp /home/builder/packages/*/*/supertux-*.apk /home/builder/build.log /out/'
}
if [ "${1:-}" != install ] || ! ls "$OUT"/supertux-[0-9]*.apk >/dev/null 2>&1; then
    build
fi
ls -l "$OUT"/*.apk
if [ "${1:-}" = install ]; then
    # (0.7.0 needs libraqm, which 0.6.3 did not: offline, from docker)
    DEVICE="$DEVICE" sh "$ROOT/tools/device-apk.sh" libraqm
    # (in /var/tmp, the NFS root: the data are 283 MB, /tmp is 128 MB of RAM)
    for P in "$OUT"/supertux-data-*.apk "$OUT"/supertux-[0-9]*.apk; do
        ssh "$DEVICE" "cat > /var/tmp/$(basename "$P")" < "$P"
    done
    ssh "$DEVICE" "cd /var/tmp && apk add --allow-untrusted --no-network --force-broken-world ./supertux-data-*.apk ./supertux-[0-9]*.apk && rm -f ./supertux-*.apk"
fi
