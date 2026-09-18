#!/bin/sh
# pwm-map.sh -- which registers of the PWM block are real, and how wide
#
# Runs ON THE iPad, fed over ssh from the host:
#
#     ssh root@10.55.0.2 sh -s < tools/pwm-map.sh | tee logs/pwm-map.txt
#
# What the last run showed, writing 732 into the first 0x40 bytes: +0x00,
# +0x04, +0x0c, +0x10 and +0x14 keep a full 32-bit value; +0x18, +0x1c and
# +0x20 keep only bits 2..4; from +0x24 on nothing sticks at all.  So this is
# not the Samsung layout that touch_cursor.c assumed -- there are no
# TCNTB/TCMPB/TCNTO triples every 0x0c -- and channel 2's counter is not at
# +0x24.  The block has to be mapped before anything else is programmed.
#
# Method: write all-ones into a register, read back to see which bits are
# implemented, put the old value back.  Only registers that keep something are
# printed.
#
# Two things are deliberately not done:
#   - +0x08 is not written.  It took bits 12 and 15 last time, which on a
#     Samsung-ish TCON start a timer, and a timer started with a zero reload
#     value is exactly what should not be left running.  It is cleared to zero
#     at the start here, in case the last run left it going.
#   - the PWM's interrupt line (AIC hwirq 16, from the ADT) is masked first.
#     Nothing in this kernel handles it, and an unhandled line that keeps
#     asserting is a hang.

set -u

GATE_BASE=0x3f100fd8
AIC_MASK_SET=0x3f204100     # AIC 0x3f200000 + 0x4100 + (hwirq >> 5) * 4
PWM=0x33500000
WORDS=256                   # the first kilobyte

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
    echo "gate $1 @ $a -> $(val "$a")"
}

echo "== power and quiet"
gate_on 68
gate_on 83
peek w "$(printf '%x' $(( AIC_MASK_SET )))" 10000 >/dev/null  # hwirq 16: the PWM
echo "   AIC hwirq 16 masked"
peek w "$(printf '%x' $(( PWM + 0x08 )))" 0 >/dev/null
echo "   +0x08 cleared to $(val "$(printf '%x' $(( PWM + 0x08 )))")"

echo "== implemented bits, 0x000..0x3fc (all-ones in, old value back)"
i=0
found=0
while [ $i -lt $WORDS ]; do
    off=$(( i * 4 ))
    i=$(( i + 1 ))
    [ $off -eq 8 ] && continue
    a=$(printf '%x' $(( PWM + off )))
    old=$(val "$a")
    case "$old" in *[!0-9a-fA-F]*|"") echo "   +0x$(printf '%03x' $off): unreadable ($old)"; continue ;; esac
    peek w "$a" ffffffff >/dev/null
    got=$(val "$a")
    peek w "$a" "$old" >/dev/null
    if [ "$got" != "00000000" ]; then
        echo "   +0x$(printf '%03x' $off) ($a): bits $got, was $old, back to $(val "$a")"
        found=$(( found + 1 ))
    fi
    [ $(( off % 256 )) -eq 252 ] && echo "   ... through +0x$(printf '%03x' $off)"
done
echo "== $found registers keep something"

echo "== leaving it quiet"
peek w "$(printf '%x' $(( PWM + 0x08 )))" 0 >/dev/null
peek r "$PWM" 12
