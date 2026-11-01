#!/bin/sh
# Run only after USBLiter8 -> patched iBSS has reached Recovery.
#   boot-from-recovery.sh          Linux
#   boot-from-recovery.sh proxy    m1n1 alone: its proxy, for the Mac's
#                                  proxyclient (the GPU work, research/gpu.md)
set -eu
cd "$(dirname "$0")"
query=$(irecovery -q)
printf '%s\n' "$query"
# CPID + BDID say iPhone12,1; PRODUCT / MODEL are checked when irecovery
# prints them (1.0.0, the one Debian / Ubuntu ship, does not)
if ! printf '%s\n' "$query" | awk '
    $1 == "CPID:" { chip = ($2 == "0x8030") }
    $1 == "BDID:" { board = ($2 == "0x04") }
    $1 == "MODEL:" { badmodel = ($2 != "n104ap") }
    $1 == "PRODUCT:" { badproduct = ($2 != "iPhone12,1") }
    $1 == "MODE:" && $2 == "Recovery" { recovery=1 }
    END { exit !(chip && board && !badmodel && !badproduct && recovery) }
'; then
    echo 'Expected iPhone12,1 / n104ap / 8030 / 04 in patched iBSS Recovery.' >&2
    exit 1
fi
irecovery -f inputs/DeviceTree.n104.rdtr.img4
irecovery -c devicetree
irecovery -f inputs/empty.rtsc.img4
irecovery -c firmware
# the GPU firmware (gfxf), loaded and set up by iBoot as for iOS: segments,
# its RTKit boot arguments, gfx-asc segment-ranges + pre-loaded in the ADT;
# and the power-management coprocessor's (pmpf), which carries out the GPU's
# power state changes
for fw in inputs/gfx.gfxf.img4 inputs/pmp.pmpf.img4; do
    [ -f "$fw" ] || continue
    irecovery -f "$fw"
    irecovery -c firmware
done
if [ "${1:-}" = proxy ]; then
    irecovery -f out/m1n1-proxy.rkrn.img4
    irecovery -c bootx
    echo 'bootx submitted: m1n1 proxy. On the Mac: ls /dev/cu.usbmodem*'
    exit 0
fi
# the Alpine root for RAM boots, as a ramdisk (m1n1 hands it to Linux)
if [ -f out/alpine.rdsk.img4 ]; then
    irecovery -f out/alpine.rdsk.img4
    irecovery -c ramdisk
fi
irecovery -f out/m1n1-linux.rkrn.img4
irecovery -c bootx
echo 'bootx submitted. ~30 s later: ssh root@172.16.42.1  (or screen /dev/cu.usbmodemN104AP_linux1)'
