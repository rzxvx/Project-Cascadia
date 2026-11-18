#!/bin/sh
# tools/desktop/retroarch/build.sh -- Alpine's RetroArch, built for the
# iPad's touch screen and its A5.  Alpine's APKBUILD for the iPad's release
# (ALPINE=3.24) from aports, changed so that:
#
#   - the touch screen is a multi-touch one: the udev input driver's touch
#     code is compiled in (-DUDEV_TOUCH_SUPPORT; upstream leaves it out) and
#     retroarch-udev-multitouch.patch makes every finger a pointer of its
#     own -- RetroArch 1.22.2 answered a touch screen's pointer queries with
#     nothing, so an overlay's controls could not be held together;
#   - it is built for the A5 (ARMv7, Cortex-A9, NEON; --enable-neon for
#     RetroArch's NEON paths) at -O2, not for Alpine armhf's ARMv6;
#   - no Qt (--disable-qt: its desktop companion, ~30 MB of Qt 6 we do not
#     use) and none of the asset/database packages Alpine makes it depend
#     on (500 MB): tools/desktop/games-setup.sh takes what it needs;
#   - pkgrel + 100, so apk takes it over Alpine's.
#
# Built by abuild in docker for armhf (linux/arm/v6, the iPad's userland)
# into build/retroarch/ -- the better part of an hour under emulation.
# "install": onto the iPad (DEVICE=root@10.55.0.2).
set -eu
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
ALPINE="${ALPINE:-3.24}"
DEVICE="${DEVICE:-root@10.55.0.2}"
OUT="$ROOT/build/retroarch"
AP="https://gitlab.alpinelinux.org/alpine/aports/-/raw/$ALPINE-stable/community/retroarch"
build() {
mkdir -p "$OUT/src"
curl -sfL -o "$OUT/src/APKBUILD" "$AP/APKBUILD"
curl -sfL -o "$OUT/src/config.patch" "$AP/config.patch"
cp "$ROOT/tools/desktop/retroarch/retroarch-udev-multitouch.patch" "$OUT/src/"
python3 - "$OUT/src/APKBUILD" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
s = re.sub(r"^pkgrel=(\d+)$", lambda m: "pkgrel=%d" % (int(m.group(1)) + 100), s, flags=re.M)
s = re.sub(r'^depends="[^"]*"$', 'depends="libretro-core-info"', s, count=1, flags=re.M)
s = s.replace(" qt6-qtbase-dev", "")
s = re.sub(r'^(\tconfig\.patch)$', r'\1\n\tretroarch-udev-multitouch.patch', s, count=1, flags=re.M)
s = s.replace('CFLAGS="$CFLAGS -I/usr/include/directfb"',
              'CFLAGS="$CFLAGS -I/usr/include/directfb -O2 -march=armv7-a -mtune=cortex-a9 -mfpu=neon'
              ' -DUDEV_TOUCH_SUPPORT"', 1)
s = s.replace("\t\t--disable-discord \\\n", "\t\t--disable-discord \\\n\t\t--disable-qt \\\n\t\t--enable-neon \\\n", 1)
open(p, "w").write(s)
PY
for want in retroarch-udev-multitouch.patch -DUDEV_TOUCH_SUPPORT --disable-qt --enable-neon 'depends="libretro-core-info"'; do
    grep -q -e "$want" "$OUT/src/APKBUILD" || { echo "build.sh: the APKBUILD changed ($want)" >&2; exit 1; }
done
! grep -q qt6 "$OUT/src/APKBUILD" || { echo "build.sh: the APKBUILD changed (qt6)" >&2; exit 1; }
rm -f "$OUT"/*.apk
# As root (abuild -F), not a sudo user: under qemu registered without the
# C flag (Arch's qemu-user-static-binfmt) setuid does nothing.  The key goes
# in by hand: abuild-keygen -i calls doas, even as root.
docker run --rm --platform linux/arm/v6 -v "$OUT":/out "alpine:$ALPINE" sh -ec '
    apk add -q alpine-sdk >/dev/null
    cp -r /out/src /root/retroarch
    ( cd /root/retroarch && abuild-keygen -a -n >/root/build.log 2>&1 && cp /root/.abuild/*.rsa.pub /etc/apk/keys/ &&
                   abuild -F checksum >>/root/build.log 2>&1 && abuild -F -r >>/root/build.log 2>&1 ) || { tail -40 /root/build.log; cp /root/build.log /out/; exit 1; }
    cp /root/packages/*/*/retroarch-[0-9]*.apk /root/build.log /out/
    cp /root/retroarch/APKBUILD /out/APKBUILD.built'
}
# "install" takes the package already built (built first if there is none);
# without it this builds again
if [ "${1:-}" != install ] || ! ls "$OUT"/retroarch-[0-9]*.apk >/dev/null 2>&1; then
    build
fi
ls -l "$OUT"/*.apk
if [ "${1:-}" = install ]; then
    P=$(ls "$OUT"/retroarch-[0-9]*.apk | head -1)
    ssh "$DEVICE" "cat > /tmp/$(basename "$P")" < "$P"
    ssh "$DEVICE" "apk add -q --allow-untrusted /tmp/$(basename "$P") && rm -f /tmp/$(basename "$P")"
fi
