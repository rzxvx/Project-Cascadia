#!/bin/sh
# pmgr-map.sh -- where does this PMGR keep the switches for SPI1 and the PWM?
#
# Runs ON THE iPad, fed over ssh from the host:
#
#     ssh root@10.55.0.2 sh -s < tools/pmgr-map.sh | tee logs/pmgr-map.txt
#
# What the first probe (tools/touch-power-probe.sh) established:
#
#   - XNU's formula for the power-state registers is the right one.  uart0's
#     ADT gate 72 lands on 0x3f101120, which reads 0x2ff -- on -- and the
#     console plainly works.  iBoot's +8 variant would put uart0 on an off
#     register, so that reading of the disassembly was wrong.
#   - But the registers for spi1 (gate 68) and pwm (gate 83) read 0x00000000
#     and swallow writes -- and so does usb-complex's (gate 87), while USB is
#     what this ssh session is running over.  Every implemented register in
#     that array carries bit 9; an all-zero one is not an off device, it is
#     not a register.
#
# So the switches for those blocks live somewhere else in the PMGR's 28 KB.
# This dumps the whole window, then switches on every power-state-shaped
# register that is currently off, checking after each one whether SPI1 or the
# PWM started answering.  A register that changes nothing is put back.
#
# Liveness: a read of a block nobody decodes returns whatever the fabric last
# drove -- 0xd00c3ccc on one boot, 0x0015006b on the next -- so "answers" means
# "reads the same twice and differs from a read of 0x38000000, which is nothing
# at all".
#
# Only writes that switch something ON, and each one is undone if it did not
# help.  Worst case is still a hung iPad and a reflash; the log names the
# register that was being tried when it went quiet.

set -u

NOTHING=38000000
SPI1=32100000
PWM=33500000
DUMP=/tmp/pmgr-dump.txt

command -v peek >/dev/null 2>&1 || {
    echo "no peek on this root: ./cascadia build puts one in /bin"
    exit 1
}

val() { peek r "$1" 1 | awk '{ print $2 }'; }

live() {        # $1 = block base
    r1=$(val "$NOTHING"); b1=$(val "$1")
    r2=$(val "$NOTHING"); b2=$(val "$1")
    [ "$b1" = "$b2" ] && [ "$b1" != "$r1" ] && [ "$b1" != "$r2" ]
}

report() {
    printf '   SPI1 %s  PWM %s  (nothing reads %s)\n' \
        "$(val $SPI1)" "$(val $PWM)" "$(val $NOTHING)"
}

echo "== PMGR window 0x3f100000 + 0x7000"
peek r 3f100000 7168 > "$DUMP" || exit 1
cat "$DUMP"

echo "== blocks before"
report
spi_live=0; live "$SPI1" && { spi_live=1; echo "   SPI1 already answers"; }
pwm_live=0; live "$PWM"  && { pwm_live=1; echo "   PWM already answers"; }

# Power-state shape: bit 9 (and sometimes bit 8) set, target nibble 0 = off.
# busybox awk has no strtonum() and no guaranteed and(), so do it by hand.
CAND=$(awk '
    function hex(s,   i, c, v) {
        v = 0
        s = tolower(s)
        for (i = 1; i <= length(s); i++) {
            c = index("0123456789abcdef", substr(s, i, 1))
            if (c == 0) return -1
            v = v * 16 + c - 1
        }
        return v
    }
    /^[0-9a-f]+:/ {
        base = hex(substr($1, 1, length($1) - 1))
        for (i = 2; i <= NF; i++) {
            v = hex($i)
            # bit 8 or 9 set (an implemented register) and target nibble 0 (off)
            if (v >= 0 && int(v / 256) % 4 != 0 && v % 16 == 0)
                printf "%x:%08x\n", base + (i - 2) * 4, v
        }
    }' "$DUMP")

echo "== $(echo "$CAND" | grep -c :) registers look like a switched-off device"

for e in $CAND; do
    a=${e%:*}
    v=${e#*:}
    echo "-- trying $a (reads $v)"
    out=$(peek w "$a" "$(printf '%x' $(( (0x$v & 0xfffffef0) | 0xf )))")
    case "$out" in *ABORT*) echo "   $out" ;; esac
    n=0
    while [ $n -lt 50 ]; do
        s=$(val "$a")
        case "$s" in *[!0-9a-fA-F]*|"") break ;; esac
        s=$(( 0x$s ))
        [ $(( (s ^ (s >> 4)) & 0xf )) -eq 0 ] && break
        n=$(( n + 1 ))
    done
    hit=
    [ "$spi_live" = 0 ] && live "$SPI1" && { spi_live=1; hit=" SPI1"; }
    [ "$pwm_live" = 0 ] && live "$PWM"  && { pwm_live=1; hit="$hit PWM"; }
    if [ -n "$hit" ]; then
        echo "!!$hit woke up on $a -- left on"
        report
        [ "$spi_live" = 1 ] && [ "$pwm_live" = 1 ] && break
    else
        peek w "$a" "$v" >/dev/null
    fi
done

echo "== blocks after"
report
