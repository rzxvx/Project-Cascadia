# Runs ON THE iPad, fed over ssh:
#
#     ssh root@10.55.0.2 sh -s < tools/z2-reset.sh | tee logs/z2-reset.txt
#
# Per docs/research/p105-gpio.md the Z2 bootloader announces itself by pulling
# ATTN LOW *while reset is asserted*:
#
#     "assert RESET=0, wait ATTN(22) low 100ms"
#     "matches upstream apple_z2_boot: assert reset -> wait ATTN (100ms) -> upload FW"
#
# The first version of this probe held reset and then only sampled ATTN after
# releasing it, so it looked at the wrong window.  This one samples DURING the
# assert as well as after the release, for both polarities, with the rails and
# the 32 kHz clock held up by keeping /dev/hx-touch open.
#
#   GPIO21 reg 0x3fa00054  bit0 = reset output level
#   GPIO22 reg 0x3fa00058  bit0 = ATTN level (low = the chip is talking)
#   pending   0x3fa00800   bit22 = latched falling edge (sticky)

command -v peek >/dev/null 2>&1 || { echo "no peek on device"; exit 1; }
[ -e /dev/hx-touch ] || { echo "no /dev/hx-touch"; exit 1; }
pkill -f hx-touchd 2>/dev/null; sleep 1

R21=3fa00054
R22=3fa00058
PEND=3fa00800
val()  { peek r "0x$1" 1 | awk '{ print $2 }'; }
setr() { peek w "0x$R21" "$(printf '%x' $(( (0x$(val $R21) & 0xfffffffe) | $1 )))" >/dev/null; }

# sample ATTN N times, report whether it ever went low / an edge latched
watch() {                       # $1 = label, $2 = iterations
    low=0; lat=0; i=0
    while [ $i -lt $2 ]; do
        c=$(val $R22); p=$(val $PEND)
        [ $(( 0x$c & 1 )) -eq 0 ] && low=1
        [ $(( (0x$p >> 22) & 1 )) -eq 1 ] && lat=1
        i=$(( i + 1 ))
    done
    echo "      $1: ATTN-went-low=$low  edge-latched=$lat  (ATTN now $(( 0x$(val $R22) & 1 )))"
}

echo "== open /dev/hx-touch (PMU GPIO0 + rails + 32 kHz clock come up)"
exec 3<>/dev/hx-touch || { echo "open failed"; exit 1; }
sleep 1
echo "   GPIO21(reset) $(val $R21)   GPIO22(ATTN) $(val $R22)   pending $(val $PEND)"

try() {                         # $1 = level that ASSERTS reset
    a=$1; b=$(( 1 - a ))
    echo "== treating reset-asserted = $a"
    setr $b; sleep 1                       # start released
    setr $a                                # ASSERT, and watch right here
    watch "during assert ($a)" 200
    setr $b                                # release, watch too
    watch "after release ($b)" 200
}

try 0     # reset active-low  (assert = drive 0)
try 1     # reset active-high (assert = drive 1)

echo "== close /dev/hx-touch"
exec 3>&-
echo "   done"
