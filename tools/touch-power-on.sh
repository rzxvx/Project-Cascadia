#!/bin/sh
# touch-power-on.sh -- switch SPI1 and the PWM on, then look at what answers.
#
# Runs ON THE iPad, fed over ssh from the host:
#
#     ssh root@10.55.0.2 sh -s < tools/touch-power-on.sh | tee logs/touch-power-on.txt
#
# pmgr-map.sh found the mapping by brute force, and both blocks woke up on it:
#
#     power-state register = 0x3f100fd8 + <ADT clock-gates id> * 4
#
# which is ten ids below where the kernelcache disassembly put the array and
# twelve below iBoot's helper.  Every earlier attempt at powering touch wrote
# to a neighbouring device's register, or to an address with no register at
# all -- which is why gate 83 "read back enabled" and the block stayed dead.
# The array covers ids 12..90 and nothing above: the display and media blocks
# (clcd 103/127, sgx 92, isp) are switched somewhere else entirely.
#
# This switches on spi1 (68) and pwm (83), and then just reads: the SPI
# controller's register file, and the first kilobyte of the PWM, which is
# where the digitizer's 32 kHz "grape-clk" lives (ADT /arm-io/pwm/grape-clk:
# reg = 2, default-hz = 32768).  One write-and-read-back on the SPI clock
# divider, restored afterwards, to prove the register file is really there.

set -u

GATE_BASE=0x3f100fd8
NOTHING=38000000
SPI1=32100000
PWM=33500000

command -v peek >/dev/null 2>&1 || {
    echo "no peek on this root: ./cascadia build puts one in /bin"
    exit 1
}

val() { peek r "$1" 1 | awk '{ print $2 }'; }
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
    echo "gate $1 @ $a: $v -> $(val "$a")"
}

live() {
    r1=$(val "$NOTHING"); b1=$(val "$1")
    r2=$(val "$NOTHING"); b2=$(val "$1")
    [ "$b1" = "$b2" ] && [ "$b1" != "$r1" ] && [ "$b1" != "$r2" ]
}

echo "== switching on"
gate_on 68
gate_on 83
live "$SPI1" && echo "   SPI1 answers" || echo "   SPI1 still dead"
live "$PWM"  && echo "   PWM  answers" || echo "   PWM  still dead"
echo "   a read of $NOTHING, which is nothing at all: $(val $NOTHING)"

echo "== SPI1 0x32100000, 32 words"
peek r "$SPI1" 32

echo "== SPI1 write-and-read-back on the clock divider (+0x30)"
old=$(val 32100030)
peek w 32100030 64 >/dev/null
echo "   wrote 00000064, reads $(val 32100030), was $old"
peek w 32100030 "$old" >/dev/null

echo "== PWM 0x33500000, first kilobyte"
peek r "$PWM" 256

echo "== the three clock registers, untouched"
for c in 3f100010 3f100020 3f10002c; do echo "   $c = $(val $c)"; done
