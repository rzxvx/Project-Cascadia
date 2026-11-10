#!/usr/bin/env bash
#
# The NFS root with the desktop, built on this machine -- ./cascadia a5
# desktop-root.  `./cascadia a5 nfs on` then seeds a new root from it, so the
# iPad's first NFS boot already has:
#
#   XFCE with touch           tools/desktop/xfce-setup.sh
#   X, xfwm4, pixman patched  tools/desktop/{xorg-server,xfwm4,pixman}/build.sh
#   autorotate                tools/desktop/install.sh
#   Mesa on the GPU           ./cascadia a5 dev mesa (the system's OpenGL, tux-gpudemo)
#   the GPU's pack            ./cascadia a5 dev gpu (from your IPSW)
#   SuperTux 0.7.0            tools/desktop/supertux/build.sh
#   screenpad                 tools/screenpad/build.sh
#   YouTube (mpv, yt-dlp)     tools/desktop/youtube-setup.sh
#
# Each of those installs onto the running iPad over ssh; here they run
# unchanged, with tools/desktop/rootssh/ssh first on PATH: it runs what they
# send inside this root instead -- a chroot in an armhf container (docker's
# linux/arm/v6, the iPad's userland), the root itself in a docker volume, so
# owners and modes are Linux's.  The result is build/desktop-root.tar.
#
#   FRESH=1   start again from the current image (build/initramfs-root);
#             otherwise the volume from last time is reused and every step
#             runs again over it (they are all safe to repeat)
#   PUBLIC=1  the root a release ships: no GPU pack (it is made from the
#             user's own IPSW, by nfs on), its own docker volume, started
#             fresh, the package list beside it (OUT.packages)
#   OUT=      where the tar goes (build/desktop-root.tar)
#   ONLY="mesa gpu"   just those steps (xfce, patched, autorotate, mesa, gpu,
#             supertux, screenpad, youtube)
#
# The first time is long: XFCE is ~280 packages under ARM emulation, and Mesa
# and SuperTux build for the better part of an hour each unless build/ has
# them already.  The tar holds Apple's files from your IPSW (firmware, the
# GPU's pack): it is yours, not something to publish.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/build/initramfs-root"
OUT="${OUT:-$ROOT/build/desktop-root.tar}"
VOL="${VOL:-cascadia-a5-desktop-root}"
if [ "${PUBLIC:-0}" = 1 ]; then
    VOL=cascadia-a5-desktop-root-public FRESH=1
    ONLY="${ONLY:-xfce patched autorotate mesa supertux screenpad}"
fi
C=cascadia-a5-root
DEV=root@desktop-root
ALPINE="${ALPINE:-3.24}"
STEPS="${ONLY:-xfce patched autorotate mesa gpu supertux screenpad youtube}"

say()  { printf '==> %s\n' "$*"; }
fail() { printf 'error: %s\n' "$*" >&2; exit 1; }

[ -x "$SRC/sbin/p105-stage2" ] || fail "no image at $SRC -- ./cascadia a5 build first"
docker info >/dev/null 2>&1 || fail "docker is not running"

inroot() { docker exec -i "$C" sh -c "$1"; }
# everything mounted under /target, deepest first; busybox's umount has no -R
UNMOUNT='awk "\$2 ~ \"^/target/\" {print \$2}" /proc/mounts | sort -r | while read -r m; do umount -l "$m"; done'

cleanup() {
    docker exec "$C" sh -c "$UNMOUNT" >/dev/null 2>&1 || true
    docker rm -f "$C" >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker rm -f "$C" >/dev/null 2>&1 || true
[ "${PUBLIC:-0}" = 1 ] && say "PUBLIC=1: the root for a release, in volume $VOL, from scratch"
if [ "${FRESH:-0}" = 1 ]; then
    say "FRESH=1: the root starts again from $SRC"
    docker volume rm "$VOL" >/dev/null 2>&1 || true
fi
docker volume create "$VOL" >/dev/null
# --privileged: the chroot needs /proc and /dev mounted for apk's scripts
docker run -d --name "$C" --privileged --platform linux/arm/v6 \
    -v "$VOL":/target "alpine:$ALPINE" sleep infinity >/dev/null
inroot 'apk add -q tar >/dev/null'

if ! inroot 'test -x /target/sbin/p105-stage2'; then
    say "the base: the image's own tree ($SRC)"
    # COPYFILE_DISABLE: macOS tar would add ._ files for extended attributes
    ( cd "$SRC" && COPYFILE_DISABLE=1 tar --exclude './dev/*' --exclude './proc/*' \
          --exclude './sys/*' --exclude './newroot' -cf - . ) \
        | inroot 'tar -C /target -xf - && chown -R 0:0 /target'
else
    say "reusing the root in volume $VOL (FRESH=1 starts again)"
fi

inroot 'cd /target && mkdir -p dev proc sys tmp var/tmp &&
    (mountpoint -q proc || mount -t proc proc proc) &&
    (mountpoint -q dev || mount --rbind /dev dev) &&
    if [ -e etc/resolv.conf ] && [ ! -e etc/resolv.conf.cascadia ]; then cp -a etc/resolv.conf etc/resolv.conf.cascadia; fi &&
    cp /etc/resolv.conf etc/resolv.conf'

export PATH="$ROOT/tools/desktop/rootssh:$PATH" CASCADIA_ROOT_CONTAINER="$C"
export DEVICE="$DEV"

step() {
    local name=$1; shift
    case " $STEPS " in *" $name "*) ;; *) return 0 ;; esac
    say "$name"
    local t0=$SECONDS
    # once more on a failure: every step is safe to repeat, and a download
    # cut short (a 40 MB llvm package, "Connection aborted") stopped a
    # whole release once
    "$@" || { echo "    $name failed -- once more"; "$@"; } \
        || fail "step '$name' failed (above); run again to retry, ONLY=\"$name\" for just this one"
    printf '    %s: %d s\n' "$name" $((SECONDS - t0))
}

step xfce       sh -c "ssh $DEV sh -s < '$ROOT/tools/desktop/xfce-setup.sh'"
step patched    sh -c "sh '$ROOT/tools/desktop/pixman/build.sh' install &&
                       sh '$ROOT/tools/desktop/xorg-server/build.sh' install &&
                       sh '$ROOT/tools/desktop/xfwm4/build.sh' install"
step autorotate sh "$ROOT/tools/desktop/install.sh"
step mesa       bash "$ROOT/cascadia" dev mesa --install "$DEV"
step gpu        bash "$ROOT/cascadia" dev gpu --install "$DEV"
step supertux   sh "$ROOT/tools/desktop/supertux/build.sh" install
step screenpad  sh "$ROOT/tools/screenpad/build.sh" install
step youtube    sh -c "ssh $DEV sh -s < '$ROOT/tools/desktop/youtube-setup.sh'"

say "checking what the iPad will find"
# apk's own view: every package there and a world it can resolve (a
# --force-broken-world that skipped something shows up here, not on the iPad)
# (SuperTux only when it was a step: the iPad 2's root, on iOS's System
# partition, leaves it out for room -- ONLY= without supertux)
has_step() { case " $STEPS " in *" $1 "*) return 0 ;; esac; return 1; }
inroot "chroot /target apk info -e xorg-server xfce4 eudev touchegg svkbd py3-gobject3 \
    $(has_step supertux && echo supertux supertux-data) >/dev/null" \
    || fail "a package is missing from the root (apk info above)"
inroot 'chroot /target apk -s add --no-network >/dev/null' \
    || fail "apk cannot resolve the root's world -- something was installed broken (above)"
for f in usr/local/bin/desktop usr/local/bin/autorotate usr/bin/xrandr usr/bin/xinput usr/local/bin/screenpad usr/local/bin/tux-gpudemo \
         usr/local/lib/sgx-mesa/lib/libGLESv2.so.2 etc/ld-musl-armhf.path \
         $([ "${PUBLIC:-0}" = 1 ] || echo usr/local/lib/sgx2d) $(has_step youtube && echo usr/local/bin/youtube); do
    inroot "chroot /target test -e /$f" || fail "no /$f in the root"
done
echo "    ok: XFCE, X, SuperTux, screenpad, autorotate, Mesa, the GPU's pack"

if [ "${PUBLIC:-0}" = 1 ]; then
    inroot 'test ! -e /target/usr/local/lib/sgx2d' || fail "PUBLIC=1 and the root has a GPU pack"
    inroot 'chroot /target apk info -v 2>/dev/null | sort' > "$OUT.packages"
    note_pk=$(wc -l < "$OUT.packages" | tr -d ' ')
    echo "    the package list: $OUT.packages ($note_pk packages)"
fi

say "packing $OUT"
# what this root is, for ./cascadia a5 install to tell it from an older one
rev=$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)
inroot "printf 'desktop %s %s\n' '$rev' '$(date -u +%Y-%m-%d)' > /target/etc/cascadia-root"
inroot "$UNMOUNT"
inroot 'awk "\$2 ~ \"^/target/\"" /proc/mounts | grep -q . && { echo "still mounted under /target:"; grep " /target/" /proc/mounts; exit 1; }; true' \
    || fail "could not unmount the root's /proc and /dev"
inroot 'cd /target && rm -f etc/resolv.conf &&
    if [ -e etc/resolv.conf.cascadia ]; then mv etc/resolv.conf.cascadia etc/resolv.conf; fi &&
    rm -rf var/cache/apk/* tmp/* var/tmp/*'
# --one-file-system and the excludes both: nothing but the root's own files,
# even if something above left a mount behind (a /proc in here once made a
# 159 GB tar of /proc/kcore)
inroot 'tar -C /target --numeric-owner --one-file-system --exclude=./proc/* --exclude=./sys/* \
    --exclude=./dev/* --exclude=./tmp/* -cpf - .' > "$OUT.part"
mv "$OUT.part" "$OUT"
ls -lh "$OUT"
echo
echo "    A new NFS root now starts from it:  ./cascadia a5 nfs on"
echo "    (an existing ~/cascadia-root is never overwritten; FORCE_SYNC=1 reseeds it)"
