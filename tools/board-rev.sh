# Runs ON THE iPad:  ssh root@10.55.0.2 sh -s < tools/board-rev.sh
#
# The board revision the way iBEC reads it (0x20624 in iBEC.dec): GPIOs
# 0x405, 0x406, 0x407 -- pins 37, 38, 39 -- set to input with pull-up, read,
# and put back; rev = p39 << 2 | p38 << 1 | p37.  It decides which of the
# ADT's touch power and clock entries iOS gets (iBEC 0x20c52..0x20d12):
#   rev <= 1   "function-clock_enable" is the PMU's GPIO0 clock, not grape-clk
#   rev <= 2   "function-power_ldo" is the pre-EVT one (LDO 12), no power_ana
#   otherwise  the ADT as shipped: grape-clk, LDO 12 analog, LDO 19 core
command -v peek >/dev/null 2>&1 || { echo "no peek on device"; exit 1; }
val() { set -- $(peek r "0x$1" 1); echo "${2:-0}"; }
rev=0; bit=0
for addr in 3fa00094 3fa00098 3fa0009c; do
    orig=$(val $addr)
    # input (mode 0, iBoot's 0x210) with pull-up (0x180), as gpio_configure(0) + gpio_pull(1)
    peek w $addr $(printf '%x' $(( (0x$orig & 0xfffffc00) | 0x390 ))) >/dev/null
    sleep 1
    now=$(val $addr)
    peek w $addr $orig >/dev/null
    lvl=$(( 0x$now & 1 ))
    echo "  $addr  was $orig  pulled up $now  -> $lvl"
    rev=$(( rev | lvl << bit )); bit=$(( bit + 1 ))
done
echo "board rev = $rev"
