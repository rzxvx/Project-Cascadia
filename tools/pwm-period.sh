#!/bin/sh
# pwm-period.sh -- which register sets the PWM period, measured against its own counter
#
# Runs ON THE iPad, fed over ssh from the host:
#
#     ssh root@10.55.0.2 sh -s < tools/pwm-period.sh | tee logs/pwm-period.txt
#
# The map from pwm-map.sh: the block is four identical files of 0x100 --
# 0x33500000, ...100, ...200, ...300 -- and each one has
#
#     +0x00 +0x04 +0x0c +0x10 +0x14   full 32 bits
#     +0x08                           not probed (it took bits 12 and 15 once)
#     +0x18 +0x20                     bits 0,2,3,4,10,11,14
#     +0x1c                           the same plus bit 5, and bit 5 sets itself
#     +0x30                           a live counter
#
# The narrow ones look like the control register of Apple's own PWM as Linux
# knows it on later chips (drivers/pwm/pwm-apple.c: ENABLE bit 0, MODE bit 2,
# UPDATE bit 5, INVERT bit 10, OUTPUT_ENABLE bit 14) -- same shape, a few bits
# moved.  There the period is two registers, off-cycles and on-cycles.
#
# The counter at +0x30 is the way in: it moves about 0x38000 between two reads,
# which at one peek per ~10 ms is the 24 MHz this SoC runs everything at, and a
# write to the file appears to reload it.  So: put 732 (24 MHz / 32768, which
# is exactly what grape-clk wants) into one register at a time and read the
# counter.  If the counter stops going above 732, that register is the period.
#
# Nothing here enables an output; the PWM's interrupt line is masked first.

set -u

GATE_BASE=0x3f100fd8
AIC_MASK_SET=0x3f204100
AIC_TIME=3f200020
PERIOD=2dc                  # 732

command -v peek >/dev/null 2>&1 || {
    echo "no peek on this root: ./cascadia build puts one in /bin"
    exit 1
}

val() { peek r "$1" 1 | awk '{ print $2 }'; }
at() { printf '%x' $(( 0x$1 + 0x$2 )); }
gate_addr() { printf '%x' $(( GATE_BASE + $1 * 4 )); }

gate_on() {
    a=$(gate_addr "$1")
    v=$(val "$a")
    peek w "$a" "$(printf '%x' $(( (0x$v & 0xfffffef0) | 0xf )))" >/dev/null
    n=0
    while [ $n -lt 200 ]; do
        s=$(val "$a")
        case "$s" in *[!0-9a-fA-F]*|"") break ;; esac
        s=$(( 0x$s ))
        [ $(( (s ^ (s >> 4)) & 0xf )) -eq 0 ] && break
        n=$(( n + 1 ))
    done
    echo "gate $1 @ $a -> $(val "$a")"
}

samples() {                 # $1 = counter address, $2 = how many
    out=
    n=0
    while [ $n -lt $2 ]; do
        out="$out $(val "$1")"
        n=$(( n + 1 ))
    done
    echo "$out"
}

echo "== power and quiet"
gate_on 68
gate_on 83
peek w "$(printf '%x' $(( AIC_MASK_SET )))" 10000 >/dev/null
echo "   AIC hwirq 16 masked"

echo "== the counter in each file, and the 24 MHz timebase beside it"
for b in 33500000 33500100 33500200 33500300; do
    c=$(at "$b" 30)
    echo "   $b +0x30: $(samples "$c" 4)"
done
echo "   AIC time: $(samples "$AIC_TIME" 4)"
echo "   (same again, interleaved with the counter of file 2)"
echo "   aic $(val $AIC_TIME) cnt $(val "$(at 33500200 30)") aic $(val $AIC_TIME) cnt $(val "$(at 33500200 30)")"

for b in 33500000 33500200 33500300; do
    echo "== file $b: 732 into one register at a time"
    for o in 00 04 0c 10 14; do
        a=$(at "$b" "$o")
        old=$(val "$a")
        peek w "$a" "$PERIOD" >/dev/null
        echo "   +0x$o = $(val "$a") -> counter$(samples "$(at "$b" 30)" 3)"
        peek w "$a" "$old" >/dev/null
    done
    echo "   put back: $(peek r "$b" 8 | tr '\n' ' ')"
done
