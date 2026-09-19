# Runs ON THE iPad, fed over ssh:
#
#     ssh root@10.55.0.2 sh -s < tools/z2-reset.sh | tee logs/z2-reset.txt
#
# ATTN never fires and MISO is never driven -> the Z2 is powered but not
# booting.  Prime suspect: reset polarity.  The driver leaves reset at logical 0
# (dts says active-high, GPIO 21), which -- if the Z2's reset is really
# active-low -- holds the chip in reset forever.
#
# This holds /dev/hx-touch open (so the rails and the 32 kHz clock stay on),
# then drives GPIO 21 (reset) by hand in both directions with a clean pulse,
# watching GPIO 22 (ATTN) for the post-reset "I'm ready" edge the bootloader
# is supposed to send.  If a pulse in one direction wakes ATTN, reset polarity
# in the dts is inverted.
#
#   GPIO21 reg 0x3fa00054  bit0 = output level
#   GPIO22 reg 0x3fa00058  bit0 = ATTN level (low = asserted)
#   pending   0x3fa00800   bit22 = latched falling edge (sticky)

command -v peek >/dev/null 2>&1 || { echo "no peek on device"; exit 1; }
[ -e /dev/hx-touch ] || { echo "no /dev/hx-touch"; exit 1; }
pkill -f hx-touchd 2>/dev/null; sleep 1

R21=3fa00054
R22=3fa00058
PEND=3fa00800
val() { peek r "0x$1" 1 | awk '{ print $2 }'; }
attn()  { echo $(( 0x$(val $R22) & 1 )); }
latch() { echo $(( (0x$(val $PEND) >> 22) & 1 )); }

echo "== open /dev/hx-touch (rails + 32 kHz clock come up, driver sets reset=0)"
exec 3<>/dev/hx-touch || { echo "open failed"; exit 1; }
sleep 1
echo "   GPIO21(reset) reg $(val $R21)   GPIO22(ATTN) reg $(val $R22)   ATTN=$(attn) latched=$(latch)"

pulse() {                       # $1 = assert level (0 or 1)
    a=$1; b=$(( 1 - a ))
    base=$(( 0x$(val $R21) & 0xfffffffe ))
    echo "== reset pulse: drive $a (hold 10ms) then $b, then watch ATTN"
    peek w "0x$R21" "$(printf '%x' $(( base | a )))" >/dev/null; sleep 1
    peek w "0x$R21" "$(printf '%x' $(( base | b )))" >/dev/null
    low=0; lat=0; i=0
    while [ $i -lt 250 ]; do
        [ "$(attn)" = 0 ] && low=1
        [ "$(latch)" = 1 ] && lat=1
        i=$(( i + 1 ))
    done
    echo "   after pulse: ATTN-went-low=$low  edge-latched=$lat  (ATTN now $(attn), reg $(val $R22))"
}

pulse 1     # assert-high then low  (correct if reset is active-high)
pulse 0     # assert-low  then high (correct if reset is active-low)

echo "== close /dev/hx-touch (rails off)"
exec 3>&-
echo "   done"
