#!/usr/bin/env bash
# Read-only NAND device tree: the PPN controller node on (p105ap-nand-rw.dts),
# but the RAM root and no write gate open -- for probing and dumping a package
# the driver has not been tested on.  apple_ppn holds its probe until bound by
# hand; apple_ftl does not mount at load.
set -euo pipefail
cd "$(dirname "$0")/.."
Z=output/zImage-dtb
DTB=dtb/p105ap-nand-ro.dtb
mkdir -p dtb
rm -f "$DTB"
dtc -q -I dts -O dtb -i dts dts/p105ap-nand-rw.dts -o "$DTB"
args=$(fdtget "$DTB" /chosen bootargs)
args=$(python3 - "$args" <<'PY'
import sys
w = sys.argv[1].split()
assert w.count('root=/dev/ram0') == 1, w
assert not any(x.startswith(('apple_ppn.', 'apple_ftl.')) for x in w), w
w = [x for x in w if x != 'iomem=relaxed']
print(' '.join(w), 'apple_ppn.hold_probe=1 apple_ftl.auto_mount=0')
PY
)
fdtput -t s "$DTB" /chosen bootargs "$args"
test "$(fdtget "$DTB" /soc@30000000/ppn@1200000 compatible)" = apple,s5l8940x-ppn
case "$args" in *allow_write*|*enable_write*) echo "write gate in bootargs" >&2; exit 1;; esac
python3 - "$Z" "$DTB" <<'PY'
import struct, sys
z, dtb = sys.argv[1], sys.argv[2]
d = open(z, 'rb').read()
off = d.rfind(bytes.fromhex('d00dfeed'))
assert off > 0 and off + struct.unpack_from('>I', d, off + 4)[0] == len(d)
open(z, 'wb').write(d[:off] + open(dtb, 'rb').read())
PY
echo "read-only NAND device tree: $args"
