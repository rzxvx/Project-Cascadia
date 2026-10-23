#!/bin/sh
# tools/desktop/xorg-server/build.sh [install] -- Alpine's xorg-server with
# xorg-server-plane-rotation.patch (docs/research/p105-mesa.md, M41): the
# modesetting driver hands RandR's rotation to a KMS plane that turns the
# picture itself (apple-sgx's copy onto the screen) instead of a shadow
# buffer turned by glamor every frame.  Alpine's APKBUILD and patches for
# the iPad's release (ALPINE=3.24) from aports, the patch added, pkgrel +
# 103 (one more each time the patch changes, so that apk takes it over the
# one installed); built by abuild in docker for armhf
# (linux/arm/v6, the iPad's userland) into build/xorg-server/ -- the whole
# server under emulation, tens of minutes.  "install": xorg-server and
# xorg-server-common onto the iPad (DEVICE=root@10.55.0.2); X takes them
# when it is next started.
set -eu
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
ALPINE="${ALPINE:-3.24}"
DEVICE="${DEVICE:-root@10.55.0.2}"
OUT="$ROOT/build/xorg-server"
AP="https://gitlab.alpinelinux.org/alpine/aports/-/raw/$ALPINE-stable/community/xorg-server"
mkdir -p "$OUT/src"
curl -sfL -o "$OUT/src/APKBUILD" "$AP/APKBUILD"
# Alpine's own patches, as its APKBUILD names them
for p in $(grep -E '^[[:space:]]+[A-Za-z0-9._-]+\.patch$' "$OUT/src/APKBUILD" | tr -d '[:blank:]'); do
    curl -sfL -o "$OUT/src/$p" "$AP/$p"
done
cp "$ROOT/tools/desktop/xorg-server/xorg-server-plane-rotation.patch" "$OUT/src/"
python3 - "$OUT/src/APKBUILD" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
s = re.sub(r"^pkgrel=(\d+)$", lambda m: "pkgrel=%d" % (int(m.group(1)) + 103), s, flags=re.M)
# fewer jobs: the LTO links of Xorg, Xvfb, Xnest and Xephyr at once, 15
# lto1 each (-flto=auto), ran docker's 8 GB out -- two LTO threads a link,
# eight jobs (XORG_JOBS)
s = s.replace("\t\t-Db_lto=true \\\n", "\t\t-Db_lto=true \\\n\t\t-Db_lto_threads=2 \\\n")
s = s.replace("\tmeson compile -C output", "\tmeson compile -j ${XORG_JOBS:-8} -C output")
# our patch after Alpine's last source line
s = re.sub(r'^(source="[^"]*?)\n\t"$', r'\1\n\txorg-server-plane-rotation.patch\n\t"', s, count=1, flags=re.M | re.S)
open(p, "w").write(s)
PY
grep -q xorg-server-plane-rotation.patch "$OUT/src/APKBUILD" && grep -q "meson compile -j" "$OUT/src/APKBUILD" &&
    grep -q "b_lto_threads=2" "$OUT/src/APKBUILD" ||
    { echo "build.sh: the APKBUILD changed" >&2; exit 1; }
rm -f "$OUT"/*.apk
docker run --rm --platform linux/arm/v6 -v "$OUT":/out "alpine:$ALPINE" sh -ec '
    apk add -q alpine-sdk sudo >/dev/null
    adduser -D builder && addgroup builder abuild
    echo "builder ALL=(ALL) NOPASSWD: ALL" > /etc/sudoers.d/builder
    cp -r /out/src /home/builder/xorg-server && chown -R builder /home/builder/xorg-server
    su builder -c "cd ~/xorg-server && export XORG_JOBS=${XORG_JOBS:-8} && abuild-keygen -a -i -n >/dev/null 2>&1 && abuild checksum >/dev/null &&
                   abuild -r >/home/builder/build.log 2>&1" || { tail -40 /home/builder/build.log; cp /home/builder/build.log /out/; exit 1; }
    cp /home/builder/packages/*/*/xorg-server-[0-9]*.apk /home/builder/packages/*/*/xorg-server-common-[0-9]*.apk \
       /home/builder/build.log /out/
    cp /home/builder/xorg-server/APKBUILD /out/APKBUILD.built'
ls -l "$OUT"/*.apk
if [ "${1:-}" = install ]; then
    for P in "$OUT"/xorg-server-common-[0-9]*.apk "$OUT"/xorg-server-[0-9]*.apk; do
        ssh "$DEVICE" "cat > /tmp/$(basename "$P")" < "$P"
    done
    ssh "$DEVICE" "cd /tmp && apk add --allow-untrusted --no-network --force-broken-world ./xorg-server-common-[0-9]*.apk ./xorg-server-[0-9]*.apk && rm -f ./xorg-server-*.apk"
fi
