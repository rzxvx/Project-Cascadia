#!/bin/sh
# tools/desktop/labwc/build.sh [install] -- labwc, the Wayland desktop's
# compositor, as upstream released it (LABWC=0.20.3), with Alpine's APKBUILD
# for the iPad's release (ALPINE=3.24) from aports.  Alpine 3.24 has 0.20.0,
# which leaves the screen black on the iPad: with no hardware cursor (the
# apple-sgx KMS has no cursor plane) the first commit that turns the output
# on is skipped, the scene having no damage on a 0x0 output (upstream
# d36348e8, "Force the initial modeset commit to actually happen", in 0.20.2
# and later).  Built by abuild in docker for armhf (linux/arm/v6, the iPad's
# userland) into build/labwc/.  "install": onto the iPad too
# (DEVICE=root@10.55.0.2).
set -eu
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
ALPINE="${ALPINE:-3.24}"
LABWC="${LABWC:-0.20.3}"
DEVICE="${DEVICE:-root@10.55.0.2}"
OUT="$ROOT/build/labwc"
AP="https://gitlab.alpinelinux.org/alpine/aports/-/raw/$ALPINE-stable/community/labwc"
build() {
rm -rf "$OUT/src"; mkdir -p "$OUT/src"
curl -sfL -o "$OUT/src/APKBUILD" "$AP/APKBUILD"
curl -sfL -o "$OUT/src/remove-aports-git-version.patch" "$AP/remove-aports-git-version.patch"
# upstream's release in place of Alpine's, a release number of our own
# (100 + one more each time this recipe changes) so apk takes it over Alpine's
python3 - "$OUT/src/APKBUILD" "$LABWC" <<'PY'
import re, sys
p, ver = sys.argv[1], sys.argv[2]
s = open(p).read()
s = re.sub(r"^pkgver=.*$", "pkgver=" + ver, s, flags=re.M)
s = re.sub(r"^pkgrel=.*$", "pkgrel=100", s, flags=re.M)
open(p, "w").write(s)
PY
grep -q "^pkgver=$LABWC$" "$OUT/src/APKBUILD" || { echo "build.sh: the APKBUILD changed" >&2; exit 1; }
# As root (abuild -F), the key by hand: see tools/desktop/xfwm4/build.sh
docker run --rm --platform linux/arm/v6 -v "$OUT":/out "alpine:$ALPINE" sh -ec '
    apk add -q alpine-sdk >/dev/null
    cp -r /out/src /root/labwc
    ( cd /root/labwc && abuild-keygen -a -n >/root/build.log 2>&1 && cp /root/.abuild/*.rsa.pub /etc/apk/keys/ &&
                   abuild -F checksum >>/root/build.log 2>&1 && abuild -F -r >>/root/build.log 2>&1 ) || { tail -40 /root/build.log; exit 1; }
    rm -f /out/labwc-*.apk
    cp /root/packages/*/*/labwc-[0-9]*.apk /root/build.log /out/
    cp /root/labwc/APKBUILD /out/APKBUILD.built'
}
# "install" takes the package already built (built first if there is none);
# without it this builds again
if [ "${1:-}" != install ] || ! ls "$OUT"/labwc-"$LABWC"-*.apk >/dev/null 2>&1; then
    build
fi
ls -l "$OUT"/labwc-*.apk
if [ "${1:-}" = install ]; then
    P=$(ls "$OUT"/labwc-"$LABWC"-*.apk | tail -1)
    ssh "$DEVICE" "cat > /tmp/$(basename "$P") && apk add --allow-untrusted --no-network --force-broken-world /tmp/$(basename "$P") && rm /tmp/$(basename "$P")" < "$P"
fi
