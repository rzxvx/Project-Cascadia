# Runs ON THE iPad, fed over ssh:
#
#     ssh root@10.55.0.2 sh -s < tools/z2-echo.sh | tee logs/z2-echo.txt
#
# Every read through the driver comes back as our own TX ("eb 01 01 00 00",
# which is exactly the gen-1 read command apple-z2 sends).  Two readings:
#   (a) the Z2 is answering with protocol bytes that happen to match, or
#   (b) the link echoes whatever we send and the Z2 is not involved at all.
#
# Decide it by sending a pattern that means nothing to the Z2 -- a5 5a c3 3c --
# with the chip properly powered (hold /dev/hx-touch open) and CS asserted.
#   RX == a5 5a c3 3c  -> (b) everything echoes; the digitizer never responds
#   RX == 00 / ff      -> (a) the eb 01 ... was protocol-specific, chip is alive
#
# Also reads the PMU rails back through the regulator sysfs, so we can see the
# enable bits actually stuck in hardware rather than trusting ret=0.

command -v peek >/dev/null 2>&1 || { echo "no peek on device"; exit 1; }
B=32100000
CS=3fa0019c          # GPIO103 (chip select) = 0x3fa00000 + 103*4
a()   { printf '%x' $(( 0x$B + 0x$1 )); }
val() { peek r "0x$1" 1 | awk '{ print $2 }'; }
csset() { peek w "0x$CS" "$(printf '%x' $(( (0x$(val $CS) & 0xfffffffe) | $1 )))" >/dev/null; }

regs() {
    for r in /sys/class/regulator/regulator.*; do
        [ -r "$r/name" ] || continue
        n=$(cat "$r/name" 2>/dev/null)
        case "$n" in touch*|*pmuclk*) echo "   $n = $(cat "$r/state" 2>/dev/null)";; esac
    done
}

pkill -f hx-touchd 2>/dev/null; sleep 1        # a stale daemon holds the device open
echo "== regulators with the device closed"; regs
echo "== open /dev/hx-touch (PMU GPIO0 + rails + 32 kHz clock)"
exec 3<>/dev/hx-touch || { echo "open failed"; exit 1; }
sleep 1
echo "== regulators after open (read back over I2C from the PMU)"; regs
echo "   (touch_pmuclk here is the one to watch: enable returns 0 but may not stick)"

echo "== hand-driven SPI transfer, CS asserted, pattern a5 5a c3 3c"
echo "   CS reg before: $(val $CS)"
peek w "$(a 0)"  d        >/dev/null
peek w "$(a 8)"  0040000f >/dev/null
peek w "$(a 30)" 4        >/dev/null
peek w "$(a c)"  6        >/dev/null
peek w "$(a 4)"  4018     >/dev/null
peek w "$(a 34)" 4        >/dev/null
peek w "$(a 4c)" 4        >/dev/null
csset 0                                     # assert CS (drive low)
peek w "$(a 4)"  4038     >/dev/null        # 0x4018 | PIOEN
for w in a5 5a c3 3c; do peek w "$(a 10)" "$w" >/dev/null; done
st=$(val "$(a 8)")
r0=$(val "$(a 20)"); r1=$(val "$(a 20)"); r2=$(val "$(a 20)"); r3=$(val "$(a 20)")
csset 1                                     # deassert CS
peek w "$(a 4)"  4018     >/dev/null
echo "   STATUS=$st  (TX-FIFO $(( (0x$st >> 6) & 0x1f ))  RX-FIFO $(( (0x$st >> 11) & 0x1f )))"
echo "   RX = $r0 $r1 $r2 $r3"
echo "   low bytes = $(printf '%02x %02x %02x %02x' $(( 0x$r0 & 0xff )) $(( 0x$r1 & 0xff )) $(( 0x$r2 & 0xff )) $(( 0x$r3 & 0xff )))"


# Second transfer: the REAL gen-1 read command apple-z2 sends (EB 01 01 then
# zeros, 16 bytes).  The driver reads this back byte-for-byte as its own TX.
# If the wire gives zeros here too, that echo is a software artifact in the
# driver's RX path, not something the digitizer is doing.
echo "== hand-driven SPI transfer, CS asserted, the gen-1 command: eb 01 01 00 ..."
peek w "$(a 0)"  d        >/dev/null
peek w "$(a 8)"  0040000f >/dev/null
peek w "$(a 30)" 4        >/dev/null
peek w "$(a c)"  6        >/dev/null
peek w "$(a 4)"  4018     >/dev/null
peek w "$(a 34)" 10       >/dev/null      # RXCNT = 16
peek w "$(a 4c)" 10       >/dev/null      # TXCNT = 16
csset 0
peek w "$(a 4)"  4038     >/dev/null
for w in eb 01 01 0 0 0 0 0 0 0 0 0 0 0 0 0; do peek w "$(a 10)" "$w" >/dev/null; done
st2=$(val "$(a 8)")
out=""
i=0
while [ $i -lt 16 ]; do
    v=$(val "$(a 20)")
    out="$out $(printf '%02x' $(( 0x$v & 0xff )))"
    i=$(( i + 1 ))
done
csset 1
peek w "$(a 4)"  4018     >/dev/null
echo "   STATUS=$st2  (TX-FIFO $(( (0x$st2 >> 6) & 0x1f ))  RX-FIFO $(( (0x$st2 >> 11) & 0x1f )))"
echo "   RX low bytes =$out"

exec 3>&-
echo "== closed"
