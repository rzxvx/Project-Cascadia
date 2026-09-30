#!/bin/sh
# sgx-probe.sh -- is the GPU (PowerVR SGX543MP2) there, and which one is it?
#
# Runs ON THE iPad, fed over ssh from the host:
#
#     ssh root@10.55.0.2 'SGX_READ=1 sh -s' < tools/sgx-probe.sh | tee logs/sgx-probe.txt
#
# Reads, plus one kind of write: switching power domains on the way
# AppleS5L8940XIO::clock_gate_switch does -- (v & ~0x10f) | 0xf, then wait for
# bits 7:4 to follow -- and back to how they were at the end.  No clock
# register is written: GFX-CLK and GFX_SYS-CLK already run off PLL@0x18
# (~102.6 and ~128 MHz in perf state 2, docs/research/p105-gpu.md).
#
#   SGX_READ unset  switch the domains on and off again, touch no SGX register
#   SGX_READ=1      also read the master bank (proven safe 2026-09-30)
#   SGX_READ=2      also read each core's bank -- HUNG on the first read
#                   (core 0 +0x00) straight after power-on, 2026-09-30
#   SGX_INIT=1      before that, run iOS's initSGX prologue (core enable,
#                   clocks, master soft reset) -- the DDK's reset alone did
#                   NOT make the core banks answer
#   NRT=1           also switch HPERF-NRT on -- NOT needed (tested 2026-09-30:
#                   the master bank answers with only GFX_SYS and GFX on)
#
# ADT arm-io/sgx: "gpu,s5l8940x", reg 0x35100000 (96 KB) and 0x3f10b000 (4 KB),
# IRQ 49, clock and power gate 92.  Gate 92 has no register of its own; its
# parents are GFX_SYS-CLK (power state 0x3f101024) and GFX-CLK (0x3f101028).
# iBEC's reset of device 6 pulses those two together with 0x3f101020,
# HPERF-NRT (0x3f10102c, the non-real-time fabric) and 0x3f101044.  Of those
# only HPERF-NRT is off at boot, and the SGX answers without it.
#
# The register map is the DDK's for an MP part (sgx_mkif_km.h: 16 KB banks,
# SGX_REG_BANK_MASTER_INDEX 1, SGX_REG_BANK_BASE_INDEX 2):
#
#   bank 0  0x0000  broadcast, WRITE-ONLY: the DDK writes CLKGATECTL and the
#                   BIF registers here at their bare offsets to reach every
#                   core, and never reads it.  A read HANGS THE BUS -- every
#                   freeze of 2026-09-27..30 but the last was a read here.
#   bank 1  0x4000  master: +0x4000 MASTER_CORE (cores - 1; 3 after power-on),
#                   +0x4004 CLKGATECTL (per core) +0x4008 CLKGATESTATUS,
#                   +0x4010 CORE_ID, +0x4014 CORE_REVISION, +0x4020 CLKGATECTL2,
#                   +0x4024 CLKGATESTATUS2, +0x4080 SOFT_RESET, +0x4c00 BIF_CTRL
#                   (sgxmpdefs.h; the CLKGATE ones from iOS's register dump)
#   bank 2+n        core n: +0x00 CLKGATECTL +0x08 CLKGATESTATUS +0x20 CORE_ID
#                   +0x24 CORE_REVISION +0x80 SOFT_RESET       (sgx544defs.h)
#
# 2026-09-30, first answer: master CORE_ID 0x01194201, CORE_REVISION
# 0x00010202 = 1.2.2, the revision the ADT's brn_31195 predicted.
#
# A block nobody powers does not fault on a read here; it returns whatever
# the fabric last drove (docs/research/p105-pmgr-gates.md).  So "alive" is
# judged against a read of 0x38000000, where there is nothing at all.

SGX=0x35100000
GLUE=0x3f10b000
GFX_SYS=0x3f101024
GFX=0x3f101028
HPERF_NRT=0x3f10102c

rd() { v=$(peek r "$1" 2>/dev/null); v=${v##*: }; echo "0x${v:-????????}"; }
wr() { peek w "$1" "$2" >/dev/null 2>&1; }
hex() { printf '0x%08x' "$(($1))"; }

gate_on() {
    v=$(rd "$1")
    wr "$1" "$(hex "(($v) & ~0x10f) | 0xf")"
    i=0
    while [ $(( $(rd "$1") & 0xf0 )) -ne $((0xf0)) ]; do
        i=$((i + 1))
        [ $i -ge 200 ] && { echo "  $1 did not follow: $(rd "$1")"; return 1; }
    done
    echo "  $1: $v -> $(rd "$1")"
}

is_on() { [ $(( $(rd "$1") & 0xf0 )) -eq $((0xf0)) ]; }

master() {
    for o in 0x4000 0x4004 0x4008 0x4010 0x4014 0x4020 0x4024 0x4080 0x4c00; do
        printf '  %-9s %s\n' "+$o" "$(rd "$(hex "$SGX + $o")")"
    done
}

core() {
    b=$(hex "$SGX + ($1 + 2) * 0x4000")
    for o in 0x00 0x08 0x20 0x24 0x80; do
        printf '  core%d +%-5s %s\n' "$1" "$o" "$(rd "$(hex "$b + $o")")"
    done
}

decode() {
    id=$1 rev=$2 who=$3
    printf '%s: core id 0x%04x, config: cores field %d, multi %d, base %d, slc %d\n' "$who" \
        $(( (id >> 16) & 0xffff )) $(( (id >> 8) & 0xf )) $(( id & 1 )) $(( (id >> 1) & 1 )) \
        $(( (id >> 12) & 0xf ))
    printf '%s: revision designer %d, %d.%d.%d\n' "$who" \
        $(( (rev >> 24) & 0xff )) $(( (rev >> 16) & 0xff )) $(( (rev >> 8) & 0xff )) $(( rev & 0xff ))
}

restore() {
    wr $GFX "$gfx0"; wr $GFX_SYS "$sys0"; wr $HPERF_NRT "$nrt0"
    echo "  GFX $(rd $GFX), GFX_SYS $(rd $GFX_SYS), HPERF-NRT $(rd $HPERF_NRT)"
}

echo "== nothing: 0x38000000 reads $(rd 0x38000000), $(rd 0x38000000)"
sys0=$(rd $GFX_SYS) gfx0=$(rd $GFX) nrt0=$(rd $HPERF_NRT)
echo "== as booted: GFX_SYS $sys0 (clock $(rd 0x3f100074)), GFX $gfx0 (clock $(rd 0x3f100070)), HPERF-NRT $nrt0 (clock $(rd 0x3f100078))"
if ! is_on $GFX_SYS && ! is_on $GFX; then
    echo "== master bank with GFX off (expect the floating value above)"
    master
else
    echo "== GFX already on (an earlier run?) -- not reading the SGX before the domains are settled"
fi
echo "== glue block $GLUE: $(rd $GLUE) $(rd $(hex "$GLUE + 4")) $(rd $(hex "$GLUE + 8")) $(rd $(hex "$GLUE + 0xc"))"

echo "== switching on"
if [ "$NRT" = 1 ]; then
    gate_on $HPERF_NRT || exit 1
else
    echo "  HPERF-NRT left as it is ($nrt0); NRT=1 to switch it on"
fi
gate_on $GFX_SYS && gate_on $GFX || exit 1
echo "== the rest of device 6: 0x3f101020 $(rd 0x3f101020), 0x3f101044 $(rd 0x3f101044)"
echo "== glue block $GLUE: $(rd $GLUE) $(rd $(hex "$GLUE + 4")) $(rd $(hex "$GLUE + 8")) $(rd $(hex "$GLUE + 0xc"))"
if [ -z "$SGX_READ" ]; then
    echo "== not reading the SGX (SGX_READ=1 for the master bank, 2 for the cores too)"
    echo "== back as it was"
    restore
    exit 0
fi

echo "== master bank"
master
if [ "$SGX_INIT" = 1 ]; then
    # iOS 8.4.1's own bring-up, IMGSGX543.kext SGXDriver543::initSGX
    # (kernelcache 0x80bf3918, helpers 0x80bf3700 and 0x80bf3754), with every
    # clock mode 1 = forced on (iOS uses 2 = auto when auto clock gating is on):
    #   MASTER_CORE = cores - 1; MASTER_CLKGATECTL2 0x4020 = 0x155; MASTER_CLKGATECTL
    #   0x4004 = mode per core (2 bits each) -- the slave cores get no clock
    #   without this, which is why their banks hung; master soft reset with SLC
    #   set up (SLC_CTRL_BYPASS has the brn_31195 bits); then CLKGATECTL,
    #   CLKGATECTL2 and 0x310 through the broadcast bank.
    echo "== iOS initSGX: core enable, clocks, master soft reset"
    ncore=$(( ($(rd $(hex "$SGX + 0x4010")) >> 8) & 0xf ))
    [ $ncore -eq 2 ] || { echo "  CORE_ID says $ncore cores, not 2 -- stopping"; restore; exit 1; }
    wr "$(hex "$SGX + 0x4000")" 0x00000001
    wr "$(hex "$SGX + 0x4020")" 0x00000155
    wr "$(hex "$SGX + 0x4004")" 0x00000005
    wr "$(hex "$SGX + 0x4080")" 0x000005f3
    wr "$(hex "$SGX + 0x4d00")" 0x0044c000
    wr "$(hex "$SGX + 0x4d04")" 0x04001e40
    wr "$(hex "$SGX + 0x4080")" 0x00000000
    wr "$(hex "$SGX + 0x0000")" 0x10155555
    wr "$(hex "$SGX + 0x0004")" 0x05454555
    wr "$(hex "$SGX + 0x0310")" 0x00000001
    master
fi
decode "$(rd $(hex "$SGX + 0x4010"))" "$(rd $(hex "$SGX + 0x4014"))" master
# How many core banks to read comes from CORE_ID's cores field, NOT from
# MASTER_CORE: that reads 3 after power-on, which the DDK's "+ 1" would call
# four cores, and banks 4 and 5 (the window is sized for an MP4) are as empty
# as bank 0.
cores=$(( ($(rd $(hex "$SGX + 0x4010")) >> 8) & 0xf ))
echo "master: CORE_ID says $cores core(s); MASTER_CORE enable field $(( $(rd $(hex "$SGX + 0x4000")) & 3 ))"
echo "   (expected: SGX543MP2, revision 1.2.2 -- the ADT's brn_31195 is in"
echo "    sgxerrata.h only for SGX543 revisions 122 and 1221)"

if [ "$SGX_READ" = 2 ]; then
    n=0
    [ $cores -ge 1 ] && [ $cores -le 2 ] || { echo "== $cores cores is not an MP2 -- not reading core banks"; cores=0; }
    while [ $n -lt $cores ]; do
        echo "== core $n bank"
        core $n
        decode "$(rd $(hex "$SGX + ($n + 2) * 0x4000 + 0x20"))" "$(rd $(hex "$SGX + ($n + 2) * 0x4000 + 0x24"))" core$n
        n=$((n + 1))
    done
fi

echo "== back as it was"
restore
