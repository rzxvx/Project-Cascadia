#!/bin/sh
# tools/desktop/pixman/build.sh [install] -- Alpine's pixman with
# pixman-scanline-clear.patch (docs/research/p105-mesa.md, M45): the
# general compositing path clears only the scanline buffers' bytes it uses,
# not 24 KiB at every call -- the X server's software compositing of
# xfwm4's a1 frame shapes, an eighth of its time on the iPad.  Alpine's
# APKBUILD for the iPad's release (ALPINE=3.24) from aports, the patch
# added, pkgrel + 100 (one more each time the patch changes, so that apk
# takes it over the one installed), its test suite left out (hours under
# emulation); built by abuild in docker for armhf (linux/arm/v6, the iPad's
# userland) into build/pixman/, a few minutes.  "install": pixman onto the
# iPad (DEVICE=root@10.55.0.2); X and every program take it when they are
# next started.
set -eu
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
ALPINE="${ALPINE:-3.24}"
DEVICE="${DEVICE:-root@10.55.0.2}"
OUT="$ROOT/build/pixman"
AP="https://gitlab.alpinelinux.org/alpine/aports/-/raw/$ALPINE-stable/main/pixman"
mkdir -p "$OUT/src"
curl -sfL -o "$OUT/src/APKBUILD" "$AP/APKBUILD"
cp "$ROOT/tools/desktop/pixman/pixman-scanline-clear.patch" "$OUT/src/"
python3 - "$OUT/src/APKBUILD" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
s = re.sub(r"^pkgrel=(\d+)$", lambda m: "pkgrel=%d" % (int(m.group(1)) + 100), s, flags=re.M)
# the patch after the tarball (abuild applies a source's *.patch with -p1)
s = re.sub(r'^(source="[^"\n]*?\.tar\.xz)"$', r'\1\n\tpixman-scanline-clear.patch\n\t"', s, count=1,
           flags=re.M)
# no test suite: pixman's runs for hours under ARM emulation
s = re.sub(r'^(arch="all")$', r'\1\noptions="!check"', s, count=1, flags=re.M)
open(p, "w").write(s)
PY
grep -q pixman-scanline-clear.patch "$OUT/src/APKBUILD" && grep -q '^options="!check"' "$OUT/src/APKBUILD" ||
    { echo "build.sh: the APKBUILD changed" >&2; exit 1; }
rm -f "$OUT"/*.apk
docker run --rm --platform linux/arm/v6 -v "$OUT":/out "alpine:$ALPINE" sh -ec '
    apk add -q alpine-sdk sudo >/dev/null
    adduser -D builder && addgroup builder abuild
    echo "builder ALL=(ALL) NOPASSWD: ALL" > /etc/sudoers.d/builder
    cp -r /out/src /home/builder/pixman && chown -R builder /home/builder/pixman
    su builder -c "cd ~/pixman && abuild-keygen -a -i -n >/dev/null 2>&1 && abuild checksum >/dev/null &&
                   abuild -r >/home/builder/build.log 2>&1" || { tail -40 /home/builder/build.log; cp /home/builder/build.log /out/; exit 1; }
    cp /home/builder/packages/*/*/pixman-[0-9]*.apk /home/builder/build.log /out/
    cp /home/builder/pixman/APKBUILD /out/APKBUILD.built'
ls -l "$OUT"/*.apk
if [ "${1:-}" = install ]; then
    for P in "$OUT"/pixman-[0-9]*.apk; do
        ssh "$DEVICE" "cat > /tmp/$(basename "$P")" < "$P"
    done
    ssh "$DEVICE" "cd /tmp && apk add --allow-untrusted --no-network --force-broken-world ./pixman-[0-9]*.apk && rm -f ./pixman-[0-9]*.apk"
fi
