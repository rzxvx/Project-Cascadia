#!/bin/sh
# touch-power-probe.sh -- can SPI1 and the PWM block (grape-clk) be woken from Linux?
#
# Runs ON THE iPad, fed over ssh from the host:
#
#     ssh root@10.55.0.2 sh -s < tools/touch-power-probe.sh | tee logs/touch-power-probe.txt
#
# Touch is blocked on two blocks that do not answer on the bus: SPI1 at
# 0x32100000, the digitizer's bus, and the PWM block at 0x33500000, whose
# channel 2 is the digitizer's 32 kHz clock (ADT /arm-io/pwm/grape-clk:
# reg = 2, default-hz = 32768; the multi-touch node's function-clock_enable
# points at it).  Both read 0xd00c3ccc, which is what this bus returns for an
# address nobody decodes.  Background: docs/touch-next-steps.md.
#
# The one experiment that switched their gates on (the 2026-09-06 XNU hook,
# mt-hook/payload.c) left three things untested, and any one of them could
# explain its result:
#   1. it wrote the gates and read the blocks straight away.  XNU's own
#      clock_gate_switch waits until the actual state (bits 7:4) equals the
#      requested one (bits 3:0) -- a block read before that is still off;
#   2. it probed PWM at +0x300 only and never read the block's base.  An
#      offset the block does not decode looks exactly like a dead block;
#   3. that PWM read panicked XNU, so SPI1 was never read at all.
# peek survives reads of dead addresses, so here each step can just be looked at.
#
# Writes: gates 68 (SPI1) and 83 (PWM), and the three clock registers the hook
# used -- all switched ON, nothing switched off.  Worst case: a hung iPad and
# a reflash.

set -u

DEAD=d00c3ccc
GATES=0x3f101000        # XNU: PMGR + 0x1000 + id*4 (iBoot's helper is +8 off)

command -v peek >/dev/null 2>&1 || {
    echo "no peek on this root: ./cascadia build puts one in /bin"
    exit 1
}

val() { peek r "$1" 1 | awk '{ print $2 }'; }
gate_addr() { printf '%x' $(( GATES + $1 * 4 )); }

state() {
    v=$(val "$1")
    case "$v" in
        "$DEAD"|--------|"") echo "dead     $v" ;;
        *)                   echo "ANSWERS  $v" ;;
    esac
}

blocks() {
    echo "   SPI1 +0x000 (32100000): $(state 32100000)"
    echo "   PWM  +0x000 (33500000): $(state 33500000)"
    echo "   PWM  +0x300 (33500300): $(state 33500300)"
}

regs() {
    echo "   gate 68 SPI1 @ $(gate_addr 68) = $(val "$(gate_addr 68)")"
    echo "   gate 83 PWM  @ $(gate_addr 83) = $(val "$(gate_addr 83)")"
    for c in 3f100010 3f100020 3f10002c; do
        echo "   clock        @ $c = $(val $c)"
    done
}

# AppleS5L8940XIO clock_gate_switch, kernelcache 12H321 @ 0x80b8de38:
#   write (v & 0xfffffef0) | 0xf, then poll until bits 3:0 == bits 7:4
gate_on() {
    a=$(gate_addr "$1")
    v=$(( 0x$(val "$a") ))
    echo "   $(peek w "$a" "$(printf '%x' $(( (v & 0xfffffef0) | 0xf )))")"
    n=0
    while :; do
        v=$(( 0x$(val "$a") ))
        [ $(( (v ^ (v >> 4)) & 0xf )) -eq 0 ] && break
        n=$(( n + 1 ))
        if [ "$n" -ge 200 ]; then
            echo "   gate $1 never settled: $(val "$a")"
            return 1
        fi
    done
    echo "   gate $1 settled at $(val "$a") after $n polls"
}

# The 0x180 bits iBEC's clock_set ORs in (docs/research/p105-touch-clock-re.md)
clk_on() {
    v=$(( 0x$(val "$1") ))
    echo "   $(peek w "$1" "$(printf '%x' $(( v | 0x180 )))") -> reads $(val "$1")"
}

echo "== 0. as booted"
regs
blocks
echo "   all gates, id 0..87:"
peek r 3f101000 88 | sed 's/^/     /'

echo "== 1. gates 83 and 68 on, the way XNU does it"
gate_on 83
gate_on 68
blocks

echo "== 2. clocks on"
clk_on 3f100010
clk_on 3f100020
clk_on 3f10002c
blocks

echo "== 3. whatever answers, in full"
for b in 32100000 33500000; do
    case "$(state $b)" in
        ANSWERS*) peek r "$b" 20 | sed 's/^/   /' ;;
        *)        echo "   $b still dead" ;;
    esac
done
