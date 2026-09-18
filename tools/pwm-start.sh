#!/bin/sh
# pwm-start.sh -- which register starts the PWM, and what sets its period
#
# Runs ON THE iPad, fed over ssh from the host:
#
#     ssh root@10.55.0.2 sh -s < tools/pwm-start.sh | tee logs/pwm-start.txt
#
# pwm-map.sh left a clue it did not mean to leave.  While it was writing
# all-ones into every register of the block and putting each one back, the
# counter at +0x30 of files 0 and 2 was running -- 0x004e4c23, then 0x0003f7d8
# a moment later.  pwm-period.sh, which only ever wrote 732 into one wide
# register at a time, saw that counter sit at zero in all four files.  So one
# of the registers the map wrote and the period run did not -- +0x18, +0x1c or
# +0x20, the three narrow ones -- is what starts the thing.
#
# Their implemented bits (0,2,3,4,10,11,14, and bit 5 in +0x1c) are the shape of
# the control register Linux knows on later Apple chips, drivers/pwm/pwm-apple.c:
# ENABLE bit 0, MODE bit 2, UPDATE bit 5, INVERT bit 10, OUTPUT_ENABLE bit 14.
#
# So: all-ones into one narrow register at a time, and after each one read the
# counter with the AIC's 24 MHz timebase either side of it, which gives both
# "did it start" and "how fast".  Then, for whichever started it, 732 into each
# wide register in turn to find the one that is the period -- a counter that
# stops going above 732 is the answer, and 24 MHz / 732 is the 32768 Hz that
# grape-clk wants.
#
# Every write is put back.  The PWM's interrupt line is masked in the AIC first.

set -u

GATE_BASE=0x3f100fd8
AIC_MASK_SET=0x3f204100
AIC_TIME=3f200020
FILE0=33500000
CNT=33500030

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

watch() {           # counter against the timebase: aic cnt aic cnt aic
    echo "   aic $(val $AIC_TIME) cnt $(val $CNT) aic $(val $AIC_TIME) cnt $(val $CNT) aic $(val $AIC_TIME)"
}

echo "== power and quiet"
gate_on 68
gate_on 83
peek w "$(printf '%x' $(( AIC_MASK_SET )))" 10000 >/dev/null
echo "   AIC hwirq 16 masked"
echo "== before"
watch

STARTER=
for o in 18 1c 20; do
    a=$(at "$FILE0" "$o")
    old=$(val "$a")
    peek w "$a" ffffffff >/dev/null
    echo "== +0x$o = $(val "$a") (was $old)"
    watch
    armed=$(val $CNT)        # while it is still armed: afterwards it may stop
    peek w "$a" "$old" >/dev/null
    echo "   put back to $(val "$a"), counter now $(val $CNT)"
    [ -z "$STARTER" ] && [ "$armed" != "00000000" ] && STARTER=$o
done

if [ -z "$STARTER" ]; then
    echo "== nothing in +0x18/+0x1c/+0x20 started the counter on its own"
    echo "== all three at once"
    for k in 18 1c 20; do peek w "$(at "$FILE0" "$k")" ffffffff >/dev/null; done
    watch
    [ "$(val $CNT)" != "00000000" ] && STARTER="18+1c+20"
    for k in 18 1c 20; do peek w "$(at "$FILE0" "$k")" 0 >/dev/null; done
fi

echo "== starter: ${STARTER:-none}"
[ -z "$STARTER" ] && exit 0

arm() {             # put the control register(s) back into the state that ran
    for k in $(echo "$STARTER" | tr '+' ' '); do
        peek w "$(at "$FILE0" "$k")" ffffffff >/dev/null
    done
}
disarm() {
    for k in $(echo "$STARTER" | tr '+' ' '); do
        peek w "$(at "$FILE0" "$k")" 0 >/dev/null
    done
}

echo "== 732 into each wide register, control re-armed after each write"
for o in 00 04 0c 10 14; do
    disarm
    for z in 00 04 0c 10 14; do peek w "$(at "$FILE0" "$z")" 0 >/dev/null; done
    peek w "$(at "$FILE0" "$o")" 2dc >/dev/null
    arm
    echo "-- +0x$o = $(val "$(at "$FILE0" "$o")")"
    watch
done
disarm
for z in 00 04 0c 10 14; do peek w "$(at "$FILE0" "$z")" 0 >/dev/null; done
echo "== left quiet: $(peek r $FILE0 12 | tr '\n' ' ')"
