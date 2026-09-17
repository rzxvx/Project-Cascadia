#!/usr/bin/env bash
# Rebuild after the 2026-09-14 AIC1 re-arm fix + the 2026-09-15 CDC ACM swap.
set -euo pipefail
cd ~/Desktop/ipad-mini-linux

TREE=~/Desktop/linux-kernel
IBSS=~/iBSSloader
LOCK="$IBSS/build/.build.lock"

fail() { echo "    FAIL: $*"; exit 1; }

echo "==> 0/6 preflight"
# A second concurrent run is what produced a stale bundle on 2026-09-15: build-kernel.sh
# takes this lock and exits 1, and that used to be swallowed by a pipe.
if command -v lsof >/dev/null 2>&1 && [ -e "$LOCK" ] && lsof "$LOCK" >/dev/null 2>&1; then
    fail "another rebuild is already running (holding $LOCK) -- wait for it to finish"
fi
P="$IBSS/build/initramfs-root/bin/peek"
[ -x "$P" ] || fail "$P missing or not executable"
case "$(file -b "$P")" in *"ELF 32-bit"*ARM*) : ;; *) fail "peek is not a static ARM binary" ;; esac
echo "    ok: no concurrent build, peek in place"

echo "==> 1/6 dtb"
# The DTB used to be a manual step, which meant an edit to dts/p105ap.dts could
# silently not reach the device: the build happily splices whatever stale .dtb
# is lying around and nothing complains.  Build it here, and rm first because
# dtc will otherwise be skipped by a cached target.
rm -f "$IBSS/dtb/p105ap.dtb"
docker run --rm -v ~/iBSSloader:/ibss ipad-mini-linux bash -c \
    "dtc -I dts -O dtb -f /ibss/dts/p105ap.dts -o /ibss/dtb/p105ap.dtb" 2>&1 | tail -3
[ -s "$IBSS/dtb/p105ap.dtb" ] || fail "dtc produced no dtb"
[ "$IBSS/dtb/p105ap.dtb" -nt "$IBSS/dts/p105ap.dts" ] || fail "dtb is not newer than the dts"
if LC_ALL=C grep -aq initcall_debug "$IBSS/dtb/p105ap.dtb"; then fail "dtb has initcall_debug; drop it from bootargs"; fi
for s in g_cdc.host_addr g_cdc.dev_addr; do
    LC_ALL=C grep -aFq "$s" "$IBSS/dtb/p105ap.dtb" || fail "$s missing from the dtb bootargs"
done
echo "    ok: fresh dtb, gadget MACs pinned, no initcall_debug"

# No RTC on this board: without a stamp the device boots at the epoch and every
# TLS handshake fails on notBefore.  Written before the kernel build so it lands
# inside the embedded initramfs rather than a build later.
date -u "+%Y-%m-%d %H:%M:%S" > "$IBSS/build/initramfs-root/etc/build-date"
echo "    stamped /etc/build-date = $(cat "$IBSS/build/initramfs-root/etc/build-date") UTC"

echo "==> 2/6 kernel"
# NOTE: pipefail INSIDE the container shell. Without it, `... | tail` returns tail's
# status and a failed or lock-refused build silently leaves stale artifacts behind.
docker run --rm --privileged \
    -v ~/iBSSloader:/ibss \
    -v ~/Desktop/linux-kernel:/ibss/build/linux \
    ipad-mini-linux bash -c "set -o pipefail; cd /ibss && bash scripts/build-kernel.sh 2>&1 | tail -25"

echo "==> 3/6 freshness"
# The build must be newer than the config it was configured from, or we are about to
# ship something that predates the edits.
[ "$TREE/arch/arm/boot/zImage" -nt "$TREE/.config" ] || fail "zImage is older than .config -- the build did not run"
[ "$TREE/arch/arm/boot/zImage" -nt "$IBSS/config/p105ap.config" ] || fail "zImage is older than the config fragment"
[ "$TREE/usr/initramfs_data.cpio" -nt "$IBSS/build/initramfs-root/init" ] || fail "initramfs archive is older than /init"
echo "    ok: zImage and initramfs are newer than their inputs"

echo "==> 4/6 splice fresh zImage + fresh DTB"
cat "$TREE/arch/arm/boot/zImage" "$IBSS/dtb/p105ap.dtb" > output/zImage-dtb
want=$(( $(stat -f%z "$TREE/arch/arm/boot/zImage") + $(stat -f%z "$IBSS/dtb/p105ap.dtb") ))
got=$(stat -f%z output/zImage-dtb)
[ "$want" = "$got" ] || fail "zImage-dtb is $got bytes, expected $want"
echo "    ok: zImage-dtb = $got bytes"

echo "==> 5/6 bundle + loader"
docker run --rm -v "$(pwd)":/work -v ~/Desktop/linux-kernel:/kernel \
    ipad-mini-linux bash /work/build-bundle.sh 2>&1 | tail -5
cp output/linux-boot.bin output/staging-loader.bin
# Do NOT compare mtimes here: build-bundle.sh writes from inside the container and
# everything in this step lands in the same second anyway, so -nt is useless.
# Check the content instead -- build-bundle.sh lays out 32K padded loader + zImage-dtb.
bs=$(stat -f%z output/staging-bundle.bin)
[ "$bs" = "$(( got + 32768 ))" ] || fail "staging-bundle.bin is $bs bytes, expected $(( got + 32768 )) (32K loader pad + zImage-dtb)"
cmp -s <(tail -c "$got" output/staging-bundle.bin) output/zImage-dtb || fail "bundle payload does not match output/zImage-dtb"
echo "    ok: bundle = 32K loader + this exact zImage-dtb"

echo "==> 6/6 verify the changes are actually in the image"
# LC_ALL=C matters: BSD grep in a UTF-8 locale silently skips regions of a binary that
# contain invalid multibyte sequences, which is how ttyGS "went missing" on 2026-09-15.
V="$TREE/vmlinux"
CPIO="$TREE/usr/initramfs_data.cpio"
rc=0
# Every string here was checked to exist verbatim in the source it comes from;
# "CDC Composite Gadget" is cdc2.c's DRIVER_DESC and "CDC Ethernet Control
# Model (ECM)" is f_ecm.c's string table, so their presence proves g_cdc and
# its ECM half actually got linked in rather than silently dropped by kconfig.
for s in AIC1-REARM irq_rearm LATE-SMOKE ttyGS CALIB PMU-TIMER SOF-TIMER apple-usb-sof \
         "CDC Composite Gadget" "CDC Ethernet Control Model (ECM)"; do
    if LC_ALL=C grep -aFq "$s" "$V"; then echo "    ok: $s in vmlinux"; else echo "    FAIL: $s missing from vmlinux"; rc=1; fi
done
# /init lives in the initramfs archive, not as plain text in vmlinux -- check it there.
for s in "P105: init start" getty ttyGS0 10.55.0.2; do
    if LC_ALL=C grep -aFq "$s" "$CPIO"; then echo "    ok: $s in initramfs"; else echo "    FAIL: $s missing from initramfs"; rc=1; fi
done
# Gadget: exactly ONE legacy gadget may be built in, and as of 2026-09-16 it is
# g_cdc (ECM + ACM together).  g_serial and g_ether must both be off, or they
# race for the UDC and whichever loses takes the console or the network with it.
grep -q "^CONFIG_USB_CDC_COMPOSITE=y" "$TREE/.config" || { echo "    FAIL: g_cdc not built in"; rc=1; }
grep -q "^CONFIG_USB_F_ECM=y" "$TREE/.config" || { echo "    FAIL: CDC ECM function not built in"; rc=1; }
grep -q "^CONFIG_USB_F_ACM=y" "$TREE/.config" || { echo "    FAIL: CDC ACM function not built in"; rc=1; }
# NOTE: `if`, not `grep && { }`.  Under `set -e` a bare AND-list whose left side
# fails takes the whole script down, and here the left side failing is the GOOD
# case -- that is a self-inflicted "build broke" with no error message.
for bad in CONFIG_USB_G_SERIAL CONFIG_USB_ETH; do
    if grep -q "^${bad}=y" "$TREE/.config"; then
        echo "    FAIL: $bad still on -- two legacy gadgets fight for the UDC"; rc=1
    else
        echo "    ok: $bad off"
    fi
done
LC_ALL=C grep -aFq initcall_debug output/zImage-dtb && { echo "    FAIL: initcall_debug in bundle"; rc=1; } || echo "    ok: no initcall_debug"
[ "$rc" -eq 0 ] || { echo "VERIFY FAILED -- do not flash"; exit 1; }

echo
echo "Ready to flash:  ./tools/flash-rearm.sh"
ls -l output/staging-bundle.bin output/staging-loader.bin
