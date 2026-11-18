#!/usr/bin/env bash
#
# The NFS root with the desktop, built on this machine -- ./cascadia a5
# desktop-root.  `./cascadia a5 nfs on` then seeds a new root from it, so the
# iPad's first NFS boot already has:
#
#   labwc, the Wayland desktop tools/desktop/labwc/{build.sh,labwc-setup.sh}
#                             (what `desktop` and the boot start)
#   task-killer               tools/task-killer/build.sh
#   autorotate                tools/desktop/install.sh
#   Mesa on the GPU           ./cascadia a5 dev mesa (the system's OpenGL, tux-gpudemo)
#   the GPU's pack            ./cascadia a5 dev gpu (from your IPSW)
#   SuperTux 0.7.0            tools/desktop/supertux/build.sh
#   screenpad                 tools/screenpad/build.sh
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
#   ONLY="mesa gpu"   just those steps (gl, labwc, taskkiller, autorotate,
#             mesa, gpu, supertux, screenpad)
#
# BOARD=k93 (./cascadia k93 desktop-root): the iPad 2's root, whose desktop
# is still XFCE on X (tools/desktop/xfce-setup.sh and the patched
# xorg-server, xfwm4 and pixman) in place of labwc.
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
    if [ "${BOARD:-p105}" = k93 ]; then ONLY="${ONLY:-gl xfce patched autorotate mesa supertux screenpad}"
    else ONLY="${ONLY:-gl labwc taskkiller autorotate mesa supertux screenpad}"; fi
fi
C=cascadia-a5-root
DEV=root@desktop-root
ALPINE="${ALPINE:-3.24}"
if [ "${BOARD:-p105}" = k93 ]; then STEPS="${ONLY:-gl xfce patched autorotate mesa gpu supertux screenpad}"
else STEPS="${ONLY:-gl labwc taskkiller autorotate mesa gpu supertux screenpad}"; fi

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

# The image's own packages (scripts/add-base-packages.sh, add-dropbear.sh) are
# baked in without apk: named in apk's world here, so that no later `apk del`
# takes them for unneeded -- one took dropbear, Wi-Fi and Bluetooth along with
# the packages it removed (2026-11-18), and ssh was gone.
say "the image's packages, in apk's world"
inroot 'chroot /target apk add -q --no-progress dropbear dropbear-scp nfs-utils libtirpc-conf busybox-static iw wpa_supplicant fbkeyboard font-dejavu bluez dbus i2c-tools alsa-utils e2fsprogs eudev' \
    || fail "could not name the image's packages in apk's world (above)"

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

# before XFCE: stands in for Alpine's Mesa, so LLVM never comes (cascadia-gl/build.sh)
step gl         sh "$ROOT/tools/desktop/cascadia-gl/build.sh" install
step xfce       sh -c "ssh $DEV sh -s < '$ROOT/tools/desktop/xfce-setup.sh'"
step patched    sh -c "sh '$ROOT/tools/desktop/pixman/build.sh' install &&
                       sh '$ROOT/tools/desktop/xorg-server/build.sh' install &&
                       sh '$ROOT/tools/desktop/xfwm4/build.sh' install"
# the setup first: it brings labwc's dependencies (Alpine's labwc with them),
# which our labwc's offline install then finds in place
step labwc      sh -c "ssh $DEV sh -s < '$ROOT/tools/desktop/labwc/labwc-setup.sh' &&
                       sh '$ROOT/tools/desktop/labwc/build.sh' install"
step taskkiller sh -c "START=0 sh '$ROOT/tools/task-killer/build.sh' install"
step autorotate sh "$ROOT/tools/desktop/install.sh"
step mesa       bash "$ROOT/cascadia" dev mesa --install "$DEV"
step gpu        bash "$ROOT/cascadia" dev gpu --install "$DEV"
step supertux   sh "$ROOT/tools/desktop/supertux/build.sh" install
step screenpad  sh "$ROOT/tools/screenpad/build.sh" install

say "checking what the iPad will find"
# apk's own view: every package there and a world it can resolve (a
# --force-broken-world that skipped something shows up here, not on the iPad)
if [ "${BOARD:-p105}" = k93 ]; then
    must_have="cascadia-gl xorg-server xfce4-session xfce4-panel eudev touchegg svkbd supertux supertux-data py3-gobject3"
    must_files="usr/local/bin/desktop-xfce usr/bin/xrandr usr/bin/xinput"
else
    must_have="cascadia-gl labwc waybar wvkbd fuzzel foot seatd xwayland pcmanfm eudev supertux supertux-data py3-gobject3"
    must_files="usr/local/bin/desktop-labwc usr/local/bin/task-killer usr/share/backgrounds/cascadia/stars.png"
fi
inroot "chroot /target apk info -e $must_have >/dev/null" \
    || fail "a package is missing from the root (apk info above)"
# and what the root must not carry (Alpine's Mesa and LLVM, the thumbnailer's tree)
for p in mesa llvm22-libs tumbler webkit2gtk-4.1 gstreamer; do
    if inroot "chroot /target apk info -e $p >/dev/null"; then fail "the root has $p (it should not)"; fi
done
inroot 'chroot /target apk -s add --no-network >/dev/null' \
    || fail "apk cannot resolve the root's world -- something was installed broken (above)"
for f in usr/local/bin/desktop $must_files usr/local/bin/autorotate usr/local/bin/screenpad usr/local/bin/tux-gpudemo \
         usr/local/lib/sgx-mesa/lib/libGLESv2.so.2 etc/ld-musl-armhf.path \
         $([ "${PUBLIC:-0}" = 1 ] || echo usr/local/lib/sgx2d); do
    inroot "chroot /target test -e /$f" || fail "no /$f in the root"
done
echo "    ok: the desktop, SuperTux, screenpad, autorotate, Mesa, the GPU's pack"

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
