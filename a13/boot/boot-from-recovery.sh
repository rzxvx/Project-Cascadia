#!/bin/sh
# Run only after USBLiter8 -> patched iBSS has reached Recovery.
#   boot-from-recovery.sh          Linux
#   boot-from-recovery.sh proxy    m1n1 alone: its proxy, for the Mac's
#                                  proxyclient (the GPU work, research/gpu.md)
#   boot-from-recovery.sh ios      iOS 16.4's own kernel by the same path (the
#                                  GPU bisect, research/gpu.md): the root
#                                  volume's trust cache and the stock
#                                  kernelcache from IOS_DIR instead of the
#                                  empty trust cache and m1n1
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
# IBEC=1: a second stage first, as every iOS boot has one (iBoot, or iBEC for
# restores) and surrealra1n's A12/A13 tethered boots do: the same patched
# image tagged ibec (inputs/iBEC.ibec.img4: ipsw img4 create --input
# inputs/iBSS.n104.20E247.sigpatched.raw --type ibec --im4m inputs/IM4M
# --output inputs/iBEC.ibec.img4). Booting straight from iBSS may skip
# platform setup that only the second stage does (research/gpu.md, G4).
if [ "${IBEC:-0}" = 1 ]; then
    irecovery -f inputs/iBEC.ibec.img4
    tries=0
    sleep 3
    until irecovery -q 2>/dev/null | grep -q 'MODE: Recovery'; do
        tries=$((tries + 1))
        if [ "$tries" -ge 20 ]; then
            echo 'iBEC did not come back in Recovery.' >&2
            exit 1
        fi
        sleep 1
    done
    echo 'iBEC up.'
fi
IOS_DIR=${IOS_DIR:-../build/gpu/ios-recovery}
# ios: the same DeviceTree payload, wrapped (like the kernel and the trust
# cache) in the phone's own apticket.der: iBoot derives boot-manifest-hash
# from it, and iOS mounts the snapshot com.apple.os.update-<that hash>
if [ "${1:-}" = ios ]; then
    irecovery -f "$IOS_DIR/devicetree.rdtr.img4"
else
    irecovery -f inputs/DeviceTree.n104.rdtr.img4
fi
irecovery -c devicetree
# ios: the phone's own SEP firmware (Preboot /usr/standalone/firmware, the
# one iBoot loads in a local boot; SEP's ROM checks it against apticket.der,
# so it is sent as it is), without which the Data volume stays locked
if [ "${1:-}" = ios ] && [ -f "$IOS_DIR/firmware/sep-firmware.img4" ]; then
    irecovery -f "$IOS_DIR/firmware/sep-firmware.img4"
    irecovery -c rsepfirmware || echo 'rsepfirmware failed, going on' >&2
fi
if [ "${1:-}" = ios ]; then
    irecovery -f "$IOS_DIR/rootfs.rtsc.img4"
else
    irecovery -f inputs/empty.rtsc.img4
fi
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
if [ "${1:-}" = ios ]; then
    # the stock kernel with APFS's root-hash requirement off (mov w20, #1
    # -> #0 at file offset 0x313909c of the raw 20E247 kernelcache)
    irecovery -f "$IOS_DIR/kernelcache.patched.rkrn.img4"
    irecovery -c bootx
    echo 'bootx submitted: iOS kernel. Watch the screen / serial console.'
    exit 0
fi
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
