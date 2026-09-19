# Runs ON THE iPad, fed over ssh:
#
#     ssh root@10.55.0.2 sh -s < tools/z2-attn.sh | tee logs/z2-attn.txt
#
# The digitizer is powered but never drives MISO and hx-touchd always fails at
# "waiting for boot IRQ".  The boot IRQ is a falling edge on ATTN = GPIO 22,
# which reaches the CPU only through AIC hwirq 119 (the gpio block's parent).
# The AIC rearm masks every line and expects drivers to unmask on request; the
# PWM line (hwirq 16) had to be unmasked by hand, so 119 may be masked too.
#
# This does three things:
#   1. unmasks AIC hwirq 119 by hand (MASK_CLR),
#   2. runs hx-touchd,
#   3. samples GPIO 22 while it boots:
#        - 0x3fa00058 bit0   = ATTN level now (the Z2 pulls it LOW to signal)
#        - 0x3fa00058 [3:1]  = edge config (0xa = falling armed)
#        - 0x3fa00800 bit22  = latched edge, sticky until the handler acks it
#
# If unmasking 119 makes hx-touchd get past the boot IRQ -> the parent was
# masked.  If ATTN never goes low / never latches -> the Z2 isn't signalling.

command -v peek >/dev/null 2>&1 || { echo "no peek on device"; exit 1; }
pkill -f hx-touchd 2>/dev/null; sleep 1

GPIO=3fa00000
CFG=$(printf '%x' $(( 0x$GPIO + 22 * 4 )))     # 3fa00058
PEND=$(printf '%x' $(( 0x$GPIO + 0x800 )))     # 3fa00800
AIC_MASK_CLR_G3=3f20418c                        # 0x3f204180 + (119>>5)*4
val() { peek r "$1" 1 | awk '{ print $2 }'; }

echo "== baseline (device closed)"
echo "   GPIO22 cfg $(val "0x$CFG")   pending-word $(val "0x$PEND")"

echo "== unmask AIC hwirq 119 (write bit23 to MASK_CLR group 3)"
peek w "$AIC_MASK_CLR_G3" 800000 >/dev/null
echo "   done"

echo "== run hx-touchd in the background, sample GPIO22 while it boots"
hx-touchd C1F14,1 /lib/firmware/P105.mtprops /lib/firmware/syscfg.bin >/tmp/hxt.out 2>&1 &
HXT=$!

pc=""; pp=""; n=0; lowseen=0; latchseen=0
while [ $n -lt 400 ]; do
    c=$(val "0x$CFG"); p=$(val "0x$PEND")
    if [ "$c" != "$pc" ] || [ "$p" != "$pp" ]; then
        lvl=$(( 0x$c & 1 )); typ=$(( (0x$c >> 1) & 7 ))
        echo "   [$n] cfg=$c (ATTN=$lvl irqtype=$typ)  pending=$p"
        pc="$c"; pp="$p"
    fi
    [ $(( 0x$c & 1 )) -eq 0 ] && lowseen=1
    [ $(( (0x$p >> 22) & 1 )) -eq 1 ] && latchseen=1
    n=$(( n + 1 ))
done

sleep 1
kill "$HXT" 2>/dev/null; pkill -f hx-touchd 2>/dev/null
echo "== hx-touchd said:"; sed 's/^/   /' /tmp/hxt.out
echo "== summary: ATTN-went-low=$lowseen  edge-latched=$latchseen"
echo "== final GPIO22 cfg $(val "0x$CFG")  pending-word $(val "0x$PEND")"
