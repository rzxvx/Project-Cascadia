#!/usr/bin/env bash
# The NAND install's device tree, swapped into output/zImage-dtb (run in the
# build image, from the a5 directory: ./cascadia a5 build does it when
# build/install.conf says disk=nand).
#
# dts/p105ap-nand-rw.dts is p105ap.dts with the raw h2fmi node off and the PPN
# controller on.  Its command line is p105ap.dts's with root=/dev/ftl0p2 and the
# write gates open for the LwVM volume "Linux" only; stage 1
# (initramfs/sbin/p105-nand-root) mounts it, and falls back to NFS / RAM when
# there is no Linux volume yet, leaving the NAND unmounted.
set -euo pipefail
cd "$(dirname "$0")/.."
Z=output/zImage-dtb
DTB=dtb/p105ap-nand.dtb
mkdir -p dtb
rm -f "$DTB"
dtc -q -I dts -O dtb -i dts dts/p105ap-nand-rw.dts -o "$DTB"
args=$(fdtget "$DTB" /chosen bootargs)
args=$(python3 - "$args" <<'PY'
import sys
w = sys.argv[1].split()
assert w.count('root=/dev/ram0') == 1 and w.count('rw') == 1, w
assert not any(x.startswith(('apple_ppn.', 'apple_ftl.')) for x in w), w
print(' '.join('root=/dev/ftl0p2' if x == 'root=/dev/ram0' else x for x in w),
      'apple_ppn.hold_probe=1 apple_ppn.allow_write=1 apple_ftl.auto_mount=0',
      'apple_ftl.enable_write=1 apple_ftl.write_volume=Linux')
PY
)
fdtput -t s "$DTB" /chosen bootargs "$args"
test "$(fdtget "$DTB" /soc@30000000/flash-controller@1200000 status)" = disabled
test "$(fdtget "$DTB" /soc@30000000/ppn@1200000 compatible)" = apple,s5l8940x-ppn
python3 - "$Z" "$DTB" <<'PY'
import struct, sys
z, dtb = sys.argv[1], sys.argv[2]
d = open(z, 'rb').read()
off = d.rfind(bytes.fromhex('d00dfeed'))
assert off > 0, 'no DTB in ' + z
assert off + struct.unpack_from('>I', d, off + 4)[0] == len(d), 'the DTB is not the tail of ' + z
open(z, 'wb').write(d[:off] + open(dtb, 'rb').read())
PY
echo "NAND device tree: $args"
