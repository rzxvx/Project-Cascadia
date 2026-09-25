# Runs ON THE iPad:  ssh root@10.55.0.2 sh -s < tools/touch-clk-check.sh
#
# Is the digitizer's 32 kHz clock intact while the driver has it open?
#
# iBEC (0x20c52) on boards of revision <= 1 feeds the digitizer from the PMU
# instead of the SoC: it writes PMU GPIO0's config (register 0x61 + gpio,
# 0x9ff0c3d0) to 0x11 AND switches SoC GPIO 63 -- grape-clk, our clock -- off.
# Switching the SoC pin off only makes sense if both drive the same wire.
# This iPad is a later revision (the live ADT keeps function-clock_enable =
# Cmwp) and iOS never touches PMU GPIO0 on it, but apple-z2.c enables
# "pmuclk-supply" on open, which turns 0x61 from 0x11 into 0x13.  If that
# drives the shared wire, grape-clk is fought over and the digitizer runs its
# firmware -- it answers -- but never scans.
#
# GPIO 63's input bit (0x3fa000fc bit 0) sees the wire.  Sampled at random
# times, a running 32 kHz clock reads as a coin toss; a wire held by someone
# else reads constant.  Three states:
#   1  device closed, PMU GPIO0 as iBoot left it, grape-clk started by hand
#   2  z2-boot holding the device: driver's clock and pmuclk both on
#   3  closed again
# Needs /tmp/z2-boot and /tmp/mtcal.bin (see docs/research/p105-z2-boot.md).
command -v peek >/dev/null 2>&1 || { echo "no peek on device"; exit 1; }
val() { peek r "$1" 1 | awk '{ print $2 }'; }

pad() {
    s=""; n=0
    while [ $n -lt 32 ]; do
        s="$s$(( 0x$(val 3fa000fc) & 1 ))"
        n=$(( n + 1 ))
    done
    ones=$(printf '%s' "$s" | tr -cd 1 | wc -c)
    echo "   GPIO63 cfg $(val 3fa000fc)  32 samples: $s  ($ones ones)"
}

grep -q ' /sys/kernel/debug ' /proc/mounts || mount -t debugfs none /sys/kernel/debug 2>/dev/null
REGMAP=$(ls -d /sys/kernel/debug/regmap/*003c* 2>/dev/null | head -1)
pmu() {
    if [ -n "$REGMAP" ]; then
        echo "   PMU $(grep -E '^(22|23|61):' "$REGMAP/registers" | tr '\n' ' ')"
    else
        echo "   PMU: no regmap in debugfs"
    fi
}
pwm() { echo "   PWM gate $(val 3f101124)  ch2 hi $(val 33500010) lo $(val 33500014) ctl $(val 33500020)"; }

killall hx-touchd z2-boot 2>/dev/null; sleep 1

echo "== 1  device closed, grape-clk by hand"
g=$(val 3f101124)
peek w 3f101124 "$(printf '%x' $(( (0x$g & 0xfffffef0) | 0xf )))" >/dev/null
n=0; while [ $n -lt 50 ]; do
    s=$(( 0x$(val 3f101124) )); [ $(( (s ^ (s >> 4)) & 0xf )) -eq 0 ] && break; n=$(( n + 1 ))
done
peek w 33500010 16e >/dev/null
peek w 33500014 16e >/dev/null
peek w 33500020 4003 >/dev/null
pmu; pwm; pad
peek w 33500020 0 >/dev/null
echo "   channel off:"; pad

echo "== 2  z2-boot holding the device"
[ -x /tmp/z2-boot ] && [ -s /tmp/mtcal.bin ] || { echo "   need /tmp/z2-boot and /tmp/mtcal.bin"; exit 1; }
setsid /tmp/z2-boot -c /tmp/mtcal.bin -t 25 >/tmp/z2boot.log 2>&1 </dev/null &
sleep 6
pmu; pwm; pad
dmesg | grep -E 'PWRSW-EN: reg=0x61' | tail -1

echo "== 3  closed again"
while pidof z2-boot >/dev/null 2>&1; do sleep 1; done
pmu; pwm; pad

echo "== z2-boot"
grep -E 'ack|ATTN|READY|write|calibration' /tmp/z2boot.log | head -30
