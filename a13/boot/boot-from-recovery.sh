#!/bin/sh
# Run only after USBLiter8 -> patched iBSS has reached Recovery.
set -eu
cd "$(dirname "$0")"
query=$(irecovery -q)
printf '%s\n' "$query"
if ! printf '%s\n' "$query" | awk '
    $1 == "CPID:" && $2 == "0x8030" { chip=1 }
    $1 == "BDID:" && $2 == "0x04" { board=1 }
    $1 == "MODEL:" && $2 == "n104ap" { model=1 }
    $1 == "PRODUCT:" && $2 == "iPhone12,1" { product=1 }
    $1 == "MODE:" && $2 == "Recovery" { recovery=1 }
    END { exit !(chip && board && model && product && recovery) }
'; then
    echo 'Expected iPhone12,1 / n104ap / 8030 / 04 in patched iBSS Recovery.' >&2
    exit 1
fi
irecovery -f inputs/DeviceTree.n104.rdtr.img4
irecovery -c devicetree
irecovery -f inputs/empty.rtsc.img4
irecovery -c firmware
irecovery -f out/m1n1-linux.rkrn.img4
irecovery -c bootx
echo 'bootx submitted. ~30 s later: ssh root@172.16.42.1  (or screen /dev/cu.usbmodemN104AP_linux1)'
