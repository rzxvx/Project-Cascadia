#!/bin/sh
# tools/desktop/xfwm4/build.sh [install] -- Alpine's xfwm4 with
# xfwm4-bypass-client-window.patch (docs/research/p105-mesa.md, M40): a
# fullscreen window that asks to bypass the compositor (SDL's games) is
# left out of the composition, as the hint means.  Alpine's APKBUILD and
# patch for the iPad's release (ALPINE=3.24) from aports, the patch added,
# pkgrel + 101; built by abuild in docker for armhf (linux/arm/v6, the
# iPad's userland) into build/xfwm4/.  "install": onto the iPad too
# (DEVICE=root@10.55.0.2), then `xfwm4 --replace` in the session.
set -eu
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
ALPINE="${ALPINE:-3.24}"
DEVICE="${DEVICE:-root@10.55.0.2}"
OUT="$ROOT/build/xfwm4"
AP="https://gitlab.alpinelinux.org/alpine/aports/-/raw/$ALPINE-stable/community/xfwm4"
mkdir -p "$OUT/src"
curl -sfL -o "$OUT/src/APKBUILD" "$AP/APKBUILD"
curl -sfL -o "$OUT/src/compositor-render-full-screen-after-startup.patch" \
    "$AP/compositor-render-full-screen-after-startup.patch"
cp "$ROOT/tools/desktop/xfwm4/xfwm4-bypass-client-window.patch" "$OUT/src/"
# our patch in the sources, a release above Alpine's (+ 101: one more each
# time the patch changes, so that apk takes it over the one installed)
python3 - "$OUT/src/APKBUILD" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
s = re.sub(r"^pkgrel=(\d+)$", lambda m: "pkgrel=%d" % (int(m.group(1)) + 101), s, flags=re.M)
s = s.replace("\tcompositor-render-full-screen-after-startup.patch\n",
              "\tcompositor-render-full-screen-after-startup.patch\n\txfwm4-bypass-client-window.patch\n")
open(p, "w").write(s)
PY
grep -q xfwm4-bypass-client-window.patch "$OUT/src/APKBUILD" || { echo "build.sh: the APKBUILD changed" >&2; exit 1; }
docker run --rm --platform linux/arm/v6 -v "$OUT":/out "alpine:$ALPINE" sh -ec '
    apk add -q alpine-sdk sudo >/dev/null
    adduser -D builder && addgroup builder abuild
    echo "builder ALL=(ALL) NOPASSWD: ALL" > /etc/sudoers.d/builder
    cp -r /out/src /home/builder/xfwm4 && chown -R builder /home/builder/xfwm4
    su builder -c "cd ~/xfwm4 && abuild-keygen -a -i -n >/dev/null 2>&1 && abuild checksum >/dev/null &&
                   abuild -r >/home/builder/build.log 2>&1" || { tail -30 /home/builder/build.log; exit 1; }
    cp /home/builder/packages/*/*/xfwm4-[0-9]*.apk /home/builder/build.log /out/
    cp /home/builder/xfwm4/APKBUILD /out/APKBUILD.built'
ls -l "$OUT"/xfwm4-*.apk
if [ "${1:-}" = install ]; then
    P=$(ls "$OUT"/xfwm4-[0-9]*.apk | tail -1)
    ssh "$DEVICE" "cat > /tmp/$(basename "$P") && apk add --allow-untrusted --no-network --force-broken-world /tmp/$(basename "$P") && rm /tmp/$(basename "$P")" < "$P"
fi
