#!/bin/sh
# sgx-probe.sh -- is the GPU (PowerVR SGX543MP2) there, and which one is it?
#
# Runs ON THE iPad, fed over ssh from the host:
#
#     ssh root@10.55.0.2 sh -s < tools/sgx-probe.sh | tee logs/sgx-probe.txt
#
# Reads, plus one kind of write: switching the GFX power/clock domains on the
# way AppleS5L8940XIO::clock_gate_switch does -- (v & ~0x10f) | 0xf, then wait
# for bits 7:4 to follow -- and back to how they were at the end.
#
# ADT arm-io/sgx: "gpu,s5l8940x", reg 0x35100000 (96 KB) and 0x3f10b000 (4 KB),
# IRQ 49, clock and power gate 92.  Gate 92 has no register in the PMGR's
# device-clocks table (scripts/adt-pmgr-map.py prints "-"); the GFX domains
# that do are GFX_SYS-CLK (power state 0x3f101024, clock 0x3f100074) and
# GFX-CLK (0x3f101028, 0x3f100070) -- the pair iBEC pulses when it resets
# device 6.  The ADT also carries brn_31195, an IMG erratum number: the DDK's
# sgxerrata.h has it for SGX543 revisions 122 and 1221 only.
#
# Register offsets from the GPL kernel DDK (TI omap5-sgx-ddk-linux,
# hwdefs/sgx543defs.h, sgxmpdefs.h, sgx_mkif_km.h): an MP core's registers
# are 16 KB banks, the master's at +0x4000, core n at (n + 2) * 0x4000.
#
#   +0x00 CLKGATECTL  +0x08 CLKGATESTATUS  +0x20 CORE_ID  +0x24 CORE_REVISION
#   +0x28/+0x2c DESIGNER_REV_FIELD1/2      +0x80 SOFT_RESET
#   master: +0x4000 MASTER_CORE (cores enabled)  +0x4010 CORE_ID
#           +0x4014 CORE_REVISION  +0x4080 SOFT_RESET  +0x4c00 BIF_CTRL
#
# A block nobody powers does not fault on a read here; it returns whatever
# the fabric last drove (docs/research/p105-pmgr-gates.md).  So "alive" is
# judged against a read of 0x38000000, where there is nothing at all.

SGX=0x35100000
GLUE=0x3f10b000
GFX_SYS=0x3f101024
GFX=0x3f101028

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

regs() {
    for o in 0x00 0x08 0x20 0x24 0x28 0x2c 0x80; do
        printf '  %-9s' "+$o"
        for b in 0x0000 0x8000 0xc000; do
            printf ' %s' "$(rd "$(hex "$SGX + $b + $o")")"
        done
        echo
    done
    for o in 0x4000 0x4010 0x4014 0x4080 0x4c00; do
        printf '  %-9s %s\n' "+$o" "$(rd "$(hex "$SGX + $o")")"
    done
}

decode() {
    id=$1 rev=$2 who=$3
    printf '%s: core id 0x%04x, config: %d core(s), multi %d, base %d, slc %d\n' "$who" \
        $(( (id >> 16) & 0xffff )) $(( ((id >> 8) & 0xf) + 1 )) $(( id & 1 )) $(( (id >> 1) & 1 )) \
        $(( (id >> 12) & 0xf ))
    printf '%s: revision designer %d, %d.%d.%d\n' "$who" \
        $(( (rev >> 24) & 0xff )) $(( (rev >> 16) & 0xff )) $(( (rev >> 8) & 0xff )) $(( rev & 0xff ))
}

echo "== nothing: 0x38000000 reads $(rd 0x38000000), $(rd 0x38000000)"
sys0=$(rd $GFX_SYS) gfx0=$(rd $GFX)
echo "== GFX domains as booted: GFX_SYS $sys0 (clock $(rd 0x3f100074)), GFX $gfx0 (clock $(rd 0x3f100070))"
echo "== SGX as booted (columns: bank 0, core 0, core 1)"
regs
echo "== glue block $GLUE: $(rd $GLUE) $(rd $(hex "$GLUE + 4")) $(rd $(hex "$GLUE + 8")) $(rd $(hex "$GLUE + 0xc"))"

echo "== switching GFX on"
gate_on $GFX_SYS && gate_on $GFX
echo "== SGX with GFX on (columns: bank 0, core 0, core 1)"
regs
echo "== glue block $GLUE: $(rd $GLUE) $(rd $(hex "$GLUE + 4")) $(rd $(hex "$GLUE + 8")) $(rd $(hex "$GLUE + 0xc"))"

echo "== decoded"
decode "$(rd $(hex "$SGX + 0x4010"))" "$(rd $(hex "$SGX + 0x4014"))" master
decode "$(rd $(hex "$SGX + 0x8020"))" "$(rd $(hex "$SGX + 0x8024"))" core0
decode "$(rd $(hex "$SGX + 0xc020"))" "$(rd $(hex "$SGX + 0xc024"))" core1
echo "   (expected from the ADT's brn_31195: revision 1.2.2 -- sgxerrata.h has that"
echo "    erratum only for SGX543 revisions 122 and 1221; the DDK knows 122, 1221,"
echo "    141, 142, 213, 216 (PS Vita's), 303)"

echo "== back as it was"
wr $GFX "$gfx0"
wr $GFX_SYS "$sys0"
echo "  GFX $(rd $GFX), GFX_SYS $(rd $GFX_SYS)"
