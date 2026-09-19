# Runs ON THE iPad, fed over ssh:
#
#     ssh root@10.55.0.2 sh -s < tools/z2-attn.sh
#     ssh root@10.55.0.2 'cat /tmp/attn.log'     # survives an ssh drop
#
# The digitizer is alive now -- it answers on SPI with real HBPP data -- but
# hx-touchd still dies on "failed waiting for boot IRQ", and the driver only
# reads reports from the ATTN interrupt, so nothing reaches userspace.
#
# Question: does the Z2 ever pull ATTN (GPIO 22) low, and does the edge latch?
#   0x3fa00058 bit0    ATTN level (low = asserted)
#   0x3fa00058 [3:1]   edge config (5 = falling armed)
#   0x3fa00800 bit22   latched edge, sticky until the handler acks it
# Results go to /tmp/attn.log so an ssh drop does not lose them.

exec >/tmp/attn.log 2>&1

command -v peek >/dev/null 2>&1 || { echo "no peek on device"; exit 1; }
pkill -f hx-touchd 2>/dev/null; sleep 1

CFG=3fa00058
PEND=3fa00800
AIC_MASK_CLR_G3=3f20418c
val() { peek r "0x$1" 1 | awk '{ print $2 }'; }

echo "== baseline (device closed)"
echo "   GPIO22 cfg $(val $CFG)   pending $(val $PEND)"

echo "== unmask AIC hwirq 119 (GPIO block's parent)"
peek w "$AIC_MASK_CLR_G3" 800000 >/dev/null

echo "== start hx-touchd, then sample ATTN hard"
hx-touchd C1F14,1 /lib/firmware/P105.mtprops /lib/firmware/syscfg.bin >/tmp/hxt.out 2>&1 &

low=0; lat=0; n=0; changes=0; pc=""
while [ $n -lt 1500 ]; do
    c=$(val $CFG)
    [ $(( 0x$c & 1 )) -eq 0 ] && low=1
    p=$(val $PEND)
    [ $(( (0x$p >> 22) & 1 )) -eq 1 ] && lat=1
    if [ "$c" != "$pc" ]; then
        changes=$(( changes + 1 ))
        [ $changes -lt 12 ] && echo "   [$n] cfg=$c pending=$p"
        pc="$c"
    fi
    n=$(( n + 1 ))
done

sleep 1
pkill -f hx-touchd 2>/dev/null
echo "== hx-touchd said:"; sed 's/^/   /' /tmp/hxt.out
echo "== samples=$n  ATTN-went-low=$low  edge-latched=$lat  cfg-changes=$changes"
echo "== final cfg $(val $CFG)  pending $(val $PEND)"
echo "== done"
