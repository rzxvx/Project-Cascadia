#!/bin/sh
# mesa/devbuild.sh [release] -- a build of the driver beside the system's, to
# try a change (docs/research/p105-mesa.md, M36): mesa/build.sh in the
# cascadia-mesa image, unpacked on the iPad into /root/devbuilds/N (rel-N
# for a release build) with /root/sgx-mesa-cur (or /root/sgx-mesa-rel)
# pointing at it.  The system's Mesa (/usr/local/lib/sgx-mesa, ./cascadia
# mesa) is left alone.  It also puts sgx-dev and hangs.sh in /root:
#
#   /root/sgx-dev PROGRAM [ARGS]          PROGRAM with /root/sgx-mesa-cur
#   SGX_MESA=/root/sgx-mesa-rel /root/sgx-dev ...     another build
#   T=$(/root/hangs.sh now); ...; sleep 3; /root/hangs.sh $T
#                                         renders that timed out since
#
# DEVICE=root@10.55.0.2 (the default) says where.  The build's log:
# build/mesa/devbuild.log; its errors are printed when it fails.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEVICE="${DEVICE:-root@10.55.0.2}"
LOG="$ROOT/build/mesa/devbuild.log"
if [ "${1:-}" = release ]; then
    MB=release; ID=install-release; PRE=rel-; LINK=/root/sgx-mesa-rel
else
    MB=; ID=install; PRE=; LINK=/root/sgx-mesa-cur
fi
mkdir -p "$ROOT/build/mesa"
docker build -q --platform linux/arm/v7 -t cascadia-mesa "$ROOT/mesa" >/dev/null
if ! docker run --rm --init --platform linux/arm/v7 -v "$ROOT":/cascadia -w /cascadia \
        -e MESA_BUILD="$MB" cascadia-mesa sh mesa/build.sh >"$LOG" 2>&1; then
    sed 's/\x1b\[[0-9;]*m//g; s/\x1b\[K//g' "$LOG" | grep -E 'error' | grep -v '^ninja' |
        cut -c1-220 | head -20
    echo "devbuild: the build failed (all of it: $LOG)" >&2
    exit 1
fi
sed 's/\x1b\[[0-9;]*m//g; s/\x1b\[K//g' "$LOG" | grep -E 'sgx/.*warning' | cut -c1-220 | head || true
N=$PRE$(date +%s)
COPYFILE_DISABLE=1 tar cf - -C "$ROOT/build/mesa/$ID/usr/local/lib" sgx-mesa |
    ssh "$DEVICE" "mkdir -p /root/devbuilds/$N && tar xf - -C /root/devbuilds/$N &&
                   ln -sfn /root/devbuilds/$N/sgx-mesa $LINK"
for f in mesa/sgx-dev tools/sgx/hangs.sh; do
    ssh "$DEVICE" "cat > /root/$(basename "$f") && chmod 755 /root/$(basename "$f")" < "$ROOT/$f"
done
echo "installed: /root/devbuilds/$N ($LINK)"
