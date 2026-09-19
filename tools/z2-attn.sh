# Runs ON THE iPad, detached, so an ssh drop cannot kill it halfway:
#
#     ssh root@10.55.0.2 'cat > /tmp/z2-attn.sh' < tools/z2-attn.sh
#     ssh root@10.55.0.2 'setsid sh /tmp/z2-attn.sh </dev/null >/dev/null 2>&1 &'
#     sleep 90; ssh root@10.55.0.2 'cat /tmp/attn.log' | tee logs/z2-attn3.txt
#
# Keep a finger moving on the glass for the whole run.
#
# Question: once hx-touchd has uploaded the firmware, does the Z2 ever pull
# ATTN (GPIO 22) low, and does anything reach the kernel?
#   0x3fa00058 bit0    ATTN level (low = asserted)
#   0x3fa00058 [3:1]   edge config (5 = falling armed)
#   0x3fa00800 bit22   latched edge, sticky until the handler acks it
#   /proc/interrupts   the apple-z2 line counts every edge the kernel took
#
# Time-bounded, not sample-bounded: one `peek` costs ~175 ms here
# (docs/research/p105-pwm-block.md), so the old 1500-sample loop ran for
# about ten minutes and every early `cat` of the log looked truncated.
#
# "failed waiting for boot IRQ" from hx-touchd is only a warning: it goes on
# and uploads the firmware anyway (bootload, 0x10d52 in the binary branches
# back into the upload).  Most of the window below is after that upload.

exec >/tmp/attn.log 2>&1
SECS=${SECS:-75}

command -v peek >/dev/null 2>&1 || { echo "no peek on device"; exit 1; }
pkill -f hx-touchd 2>/dev/null; sleep 1

CFG=3fa00058
PEND=3fa00800
AIC_MASK_CLR_G3=3f20418c
val() { set -- $(peek r "0x$1" 1); echo "${2:-0}"; }
now() { read -r u _ </proc/uptime; echo "${u%.*}"; }
irqs() { l=$(grep apple-z2 /proc/interrupts | tr -s ' '); echo "${l:-(no apple-z2 line in /proc/interrupts)}"; }

echo "== baseline (device closed)"
echo "   GPIO22 cfg $(val $CFG)   pending $(val $PEND)"
echo "   $(irqs)"

echo "== unmask AIC hwirq 119 (GPIO block's parent)"
peek w "$AIC_MASK_CLR_G3" 800000 >/dev/null

echo "== start hx-touchd, sample ATTN for ${SECS}s -- finger on the glass"
hx-touchd C1F14,1 /lib/firmware/P105.mtprops /lib/firmware/syscfg.bin >/tmp/hxt.out 2>&1 &

t0=$(now); n=0; low=0; lat=0; changes=0; pc=""
while [ $(( $(now) - t0 )) -lt "$SECS" ]; do
    c=$(val $CFG); p=$(val $PEND)
    [ $(( 0x$c & 1 )) -eq 0 ] && low=$(( low + 1 ))
    [ $(( (0x$p >> 22) & 1 )) -eq 1 ] && lat=$(( lat + 1 ))
    if [ "$c" != "$pc" ]; then
        changes=$(( changes + 1 ))
        [ $changes -le 12 ] && echo "   [t+$(( $(now) - t0 ))s] cfg=$c pending=$p"
        pc="$c"
    fi
    n=$(( n + 1 ))
done

echo "== after ${SECS}s"
echo "   $(irqs)"
pkill -f hx-touchd 2>/dev/null; sleep 1
echo "== hx-touchd said:"; sed 's/^/   /' /tmp/hxt.out
echo "== samples=$n  ATTN-low-samples=$low  latched-samples=$lat  cfg-changes=$changes"
echo "== driver, last lines:"
dmesg | grep -iE 'apple-z2|z2-open|hx-touch|packet|checksum|read header' | tail -12 | sed 's/^/   /'
echo "== done"
