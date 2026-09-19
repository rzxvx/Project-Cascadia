#!/bin/sh
# pwm-find-pin.sh -- watch every GPIO pad while the PWM channel is switched on
#
# Runs ON THE iPad, fed over ssh from the host:
#
#     ssh root@10.55.0.2 sh -s < tools/pwm-find-pin.sh | tee logs/pwm-find-pin.txt
#
# The iOS recipe for a channel is three writes (AppleS5L8920XPWM, kernelcache
# 12H321): cycles into ch*8 and ch*8+4, then 0x4003 into 0x18 + ch*4.  Running
# it on channel 2 -- which is what the ADT's grape-clk asks for, reg = 2 --
# leaves the capture registers of that channel dead, but those capture an input
# edge, and nothing is attached to answer: the digitizer has no power yet.  So
# the output may well be running with nothing to show for it.
#
# The pads can show it.  The GPIO block at 0x3fa00000 has one register per pin,
# 256 of them, and the pad state is in there.  A pin carrying a 32 kHz square
# wave read at ~10 ms per peek comes back as a coin toss; a pin that is doing
# nothing reads the same value every time.  So: snapshot all 256 registers six
# times with the channel off, six times with it on, six times off again, and
# report the pins that only move in the middle.  That both proves the clock is
# running and names the pin it comes out of.

set -u

GATE_BASE=0x3f100fd8
AIC_MASK_SET=0x3f204100
GPIO=3fa00000
F=33500000
HALF=16e                    # 366 + 366 = 732 ticks of 24 MHz = 32787 Hz
ON=4003                     # what iOS writes
T=/tmp/pwmpin

command -v peek >/dev/null 2>&1 || {
    echo "no peek on this root: ./cascadia build puts one in /bin"
    exit 1
}

val() { peek r "$1" 1 | awk '{ print $2 }'; }
at() { printf '%x' $(( 0x$F + $1 )); }
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

snaps() {                   # $1 = tag: six snapshots of every pad register
    rm -f "$T".*
    i=1
    while [ $i -le 6 ]; do
        peek r "$GPIO" 256 | awk '{ for (k = 2; k <= NF; k++) print $k }' > "$T.$i"
        i=$(( i + 1 ))
    done
    echo "-- $1: pins that are not the same in all six reads"
    awk '{ v[FNR] = v[FNR] " " $0; if (FNR > n) n = FNR }
         END {
             for (i = 1; i <= n; i++) {
                 c = split(v[i], a, " ")
                 same = 1
                 for (j = 2; j <= c; j++) if (a[j] != a[1]) same = 0
                 if (!same) printf "   pin %3d (%s+0x%03x):%s\n", i - 1, "'"$GPIO"'", (i - 1) * 4, v[i]
             }
         }' "$T".1 "$T".2 "$T".3 "$T".4 "$T".5 "$T".6
}

ch_on() {                   # $1 = channel
    peek w "$(at $(( $1 * 8 )))" "$HALF" >/dev/null
    peek w "$(at $(( $1 * 8 + 4 )))" "$HALF" >/dev/null
    peek w "$(at $(( 24 + $1 * 4 )))" "$ON" >/dev/null
    echo "== channel $1 on: control $(val "$(at $(( 24 + $1 * 4 )))")"
}
ch_off() {
    peek w "$(at $(( 24 + $1 * 4 )))" 0 >/dev/null
    peek w "$(at $(( $1 * 8 )))" 0 >/dev/null
    peek w "$(at $(( $1 * 8 + 4 )))" 0 >/dev/null
    echo "== channel $1 off"
}

echo "== power and quiet"
gate_on 68
gate_on 83
peek w "$(printf '%x' $(( AIC_MASK_SET )))" 10000 >/dev/null
echo "   AIC hwirq 16 masked"

snaps "everything off"
ch_on 2
snaps "channel 2 running"
ch_off 2
snaps "channel 2 off again"
ch_on 0
snaps "channel 0 running"
ch_off 0
rm -f "$T".*
