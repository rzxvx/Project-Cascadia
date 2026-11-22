#!/usr/bin/env bash
# rootfs-sync.sh - ./cascadia a10 rootfs sync: bring the Alpine root the iPad
# runs (on the NVMe, or in RAM until the next boot) up to this tree:
# rootfs/packages installed with apk, rootfs/overlay/ copied over /. What
# build rootfs puts into a new root, for one that is already there.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
A10="$(cd "$HERE/.." && pwd)"
HOST=${HOST:-root@172.16.43.1}
ipad() { ssh -o ConnectTimeout=8 -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR "$HOST" "$@"; }

ipad true </dev/null 2>/dev/null || { echo "error: no Linux on the iPad at ${HOST#*@}" >&2; exit 1; }
root=$(ipad 'grep " / " /proc/mounts | cut -d" " -f1' </dev/null)
echo "==> the iPad's root: $root"
case "$root" in /dev/nvme*) ;; *) echo "    (not on the NVMe: this lasts until the next boot)" ;; esac
echo "==> apk add (rootfs/packages, rootfs/packages-desktop)"
ipad "apk add -q $(cat "$A10/rootfs/packages" "$A10/rootfs/packages-desktop" | tr '\n' ' ')" </dev/null
echo "==> rootfs/overlay onto /"
# a tar busybox reads: ustar, no macOS extended attributes
( cd "$A10/rootfs/overlay" && COPYFILE_DISABLE=1 tar --format ustar --no-xattrs --uid 0 --gid 0 --uname root --gname root -cf - . ) \
    | ipad 'tar xf - -C / && chmod 755 /sbin/a10-init /usr/local/bin/desktop /usr/local/bin/kbd-toggle && sync'
# sync: the iPad only ever goes off by a hard reset (DFU, the buttons), and
# ext4 brought files rewritten here back empty after one
echo "==> task-killer"
[ -x "$A10/build/task-killer/task-killer" ] || sh "$A10/tools/task-killer/build.sh"
ipad 'cat > /usr/local/bin/task-killer.new && chmod 755 /usr/local/bin/task-killer.new &&
      mv /usr/local/bin/task-killer.new /usr/local/bin/task-killer && sync' \
    < "$A10/build/task-killer/task-killer"
echo "==> done"
