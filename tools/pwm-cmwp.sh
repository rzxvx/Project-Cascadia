#!/bin/sh
# pwm-cmwp.sh -- drive the PWM the way iOS drives it, and measure the result
#
# Runs ON THE iPad, fed over ssh from the host:
#
#     ssh root@10.55.0.2 sh -s < tools/pwm-cmwp.sh | tee logs/pwm-cmwp.txt
#
# The recipe is no longer a guess.  AppleS5L8920XPWM in the 12H321 kernelcache
# (vtable slot +0x340, code at 0x80909f78) does exactly three writes to enable a
# channel:
#
#     write(ch * 8 + 0, cycles_a)
#     write(ch * 8 + 4, cycles_b)
#     write(0x18 + ch * 4, 0x4003)        (0x4207 in the other mode)
#
# and its capture path (slot +0x350, 0x8090a08c) writes 0x801 to the same
# control register and then reads a pair at 0x24 + ch*8 and 0x28 + ch*8 --
# timestamps, which slot +0x358 subtracts from the current time.
#
# That lands on top of everything measured from this end: three channels per
# 0x100 file, cycle registers at 0x00/0x04, 0x08/0x0c, 0x10/0x14, control at
# 0x18, 0x1c, 0x20 -- and the counter that has been moving at +0x30 all along
# is channel 1's second capture register, 0x28 + 1*8, which is why it only ever
# ran when +0x1c had bits written into it.
#
# So: 366 and 366 into a channel's pair -- 732 ticks of 24 MHz, the 32787 Hz
# that grape-clk asks for -- then 0x4803 into its control, which is the enable
# value with the capture bit added so the edges timestamp themselves.  If the
# output is running, the two capture registers move together about 24 million
# per second and sit about 366 apart.  If it is not, they do not move at all.
#
# The ADT's grape-clk is reg = 2, so channel 2 -- pair 0x10/0x14, control 0x20,
# captures 0x34/0x38 -- is the one that matters.  All three are tried.

set -u

GATE_BASE=0x3f100fd8
AIC_MASK_SET=0x3f204100
AIC_TIME=3f200020
F=33500000
HALF=16e                    # 366
ENABLE=4803                 # 0x4003 (what iOS writes) + 0x800 (capture)

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

quiet() {              # offsets 0x00..0x20, in decimal for $(( ))
    for k in 0 4 8 12 16 20 24 28 32; do peek w "$(at $k)" 0 >/dev/null; done
}

channel() {                 # $1 = channel number
    ch=$1
    a=$(at $(( ch * 8 )))
    b=$(at $(( ch * 8 + 4 )))
    c=$(at $(( 24 + ch * 4 )))          # 0x18 + ch*4
    p=$(at $(( 36 + ch * 8 )))          # 0x24 + ch*8
    q=$(at $(( 40 + ch * 8 )))          # 0x28 + ch*8
    quiet
    peek w "$a" "$HALF" >/dev/null
    peek w "$b" "$HALF" >/dev/null
    peek w "$c" "$ENABLE" >/dev/null
    echo "== channel $ch: cycles $a=$(val "$a") $b=$(val "$b"), control $c=$(val "$c")"
    t0=$(val $AIC_TIME)
    for i in 1 2 3; do
        x=$(val "$p"); y=$(val "$q")
        echo "   capture $p=$x  $q=$y   (apart: $(( 0x$y - 0x$x )))"
        [ $i -lt 3 ] && sleep 1
    done
    t1=$(val $AIC_TIME)
    echo "   timebase moved $(( 0x$t1 - 0x$t0 )) ticks over those two seconds"
    peek w "$c" 0 >/dev/null
}

echo "== power and quiet"
gate_on 68
gate_on 83
peek w "$(printf '%x' $(( AIC_MASK_SET )))" 10000 >/dev/null
echo "   AIC hwirq 16 masked"

channel 0
channel 1
channel 2

quiet
echo "== left quiet: $(peek r $F 16 | tr '\n' ' ')"
