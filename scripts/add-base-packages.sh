#!/usr/bin/env bash
#
# The Alpine packages the image carries from the first boot, because each is
# needed before there is a way to install anything:
#
#   nfs-utils           mount.nfs -- an NFS root cannot be mounted by a binary
#                       that lives on it
#   busybox-static      stage 2 runs the cable console from a RAM copy of it
#   iw, wpa_supplicant  Wi-Fi.  A RAM boot has no NFS root and no network but
#                       Wi-Fi, and apk cannot fetch what Wi-Fi itself needs
#   fbkeyboard          the on-screen keyboard, the only keyboard there is
#   font-dejavu         its font.  Only DejaVuSans.ttf is kept: the package is
#                       10 MB of variants (apk add font-dejavu for the rest)
#   bluez, dbus         Bluetooth (`bluetooth`): a keyboard is the other way to
#                       type, and in a RAM boot the only one besides the glass.
#                       Their install scripts are not run here, so what they
#                       would have made is made below: the messagebus user
#                       dbus-daemon --system drops to (Alpine's uid and gid),
#                       and the empty state directories
#
# dropbear comes from add-dropbear.sh.  Called by build-alpine-rootfs.sh, and by
# ./cascadia build for a tree made before this list grew.  An NFS root is
# seeded from the same tree, so a new one has them too; one seeded earlier
# gets them with apk add.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TREE="$ROOT/build/initramfs-root"

bash "$ROOT/scripts/add-apk-packages.sh" \
    nfs-utils busybox-static iw wpa_supplicant fbkeyboard font-dejavu bluez dbus
find "$TREE/usr/share/fonts/dejavu" -type f ! -name DejaVuSans.ttf -delete
grep -q '^messagebus:' "$TREE/etc/group" \
    || echo 'messagebus:x:101:messagebus' >> "$TREE/etc/group"
grep -q '^messagebus:' "$TREE/etc/passwd" \
    || echo 'messagebus:x:100:101:messagebus:/dev/null:/sbin/nologin' >> "$TREE/etc/passwd"
mkdir -p "$TREE/var/lib/bluetooth" "$TREE/var/lib/dbus"
