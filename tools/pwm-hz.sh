#!/bin/sh
# pwm-hz.sh -- check the PWM's unit of time on the pad it comes out of
#
# Runs ON THE iPad, fed over ssh from the host:
#
#     ssh root@10.55.0.2 sh -s < tools/pwm-hz.sh | tee logs/pwm-hz.txt
#
# pwm-find-pin.sh found the digitizer's clock: with channel 2 running the iOS
# recipe, GPIO pin 63 (0x3fa000fc) reads 0x620 / 0x621 at random, and it is the
# only pad in the SoC that does -- still with the channel off, still again with
# channel 0 on instead.  A coin toss says "fast", not "32768 Hz".
#
# So slow it down to where the samples can follow it: 12,000,000 + 12,000,000
# ticks is one cycle per second if the unit is the 24 MHz clock everything else
# on this SoC runs from.  Sampled every quarter second, the pad should then read
# like 0000111100001111.  Then 6,000,000 + 18,000,000: still one second, but one
# half is three times the other, which says which of the two registers holds
# the high time.  If both come out as described, 366 + 366 is 32787 Hz, and
# that is what the pin carries when the channel is left at its real setting.

set -u

GATE_BASE=0x3f100fd8
AIC_MASK_SET=0x3f204100
AIC_TIME=3f200020
PAD=3fa000fc
F=33500000
CH=2

command -v peek >/dev/null 2>&1 || {
    echo "no peek on this root: ./cascadia build puts one in /bin"
    exit 1
}

val() { peek r "$1" 1 | awk '{ print $2 }'; }
at() { printf '%x' $(( 0x$F + $1 )); }
gate_addr() { printf '%x' $(( GATE_BASE + $1 * 4 )); }
nap() { sleep 0.25 2>/dev/null || usleep 250000; }

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

run() {                     # $1 = cycles_a, $2 = cycles_b, both hex
    peek w "$(at $(( 24 + CH * 4 )))" 0 >/dev/null
    peek w "$(at $(( CH * 8 )))" "$1" >/dev/null
    peek w "$(at $(( CH * 8 + 4 )))" "$2" >/dev/null
    peek w "$(at $(( 24 + CH * 4 )))" 4003 >/dev/null
    t0=$(val $AIC_TIME)
    bits=
    i=0
    while [ $i -lt 16 ]; do
        v=$(val $PAD)
        bits="$bits$(( 0x$v & 1 ))"
        nap
        i=$(( i + 1 ))
    done
    t1=$(val $AIC_TIME)
    echo "   a=0x$1 b=0x$2: $bits   ($(( (0x$t1 - 0x$t0) / 24000 )) ms for 16 samples)"
}

echo "== power and quiet"
gate_on 68
gate_on 83
peek w "$(printf '%x' $(( AIC_MASK_SET )))" 10000 >/dev/null
echo "   AIC hwirq 16 masked"
echo "== pad $PAD at rest: $(val $PAD)"

echo "== one second per cycle, even halves: expect runs of about four"
run b71b00 b71b00
echo "== one second per cycle, a quarter and three quarters"
run 5b8d80 112a880
echo "== the other way round"
run 112a880 5b8d80

echo "== back to 366 + 366 = 32787 Hz: the pad should go back to a coin toss"
run 16e 16e

peek w "$(at $(( 24 + CH * 4 )))" 0 >/dev/null
echo "== channel $CH off, pad $PAD: $(val $PAD)"
