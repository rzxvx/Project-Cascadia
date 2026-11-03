# Runs ON THE iPad, fed over ssh from the host:
#
#     ssh root@10.55.0.2 sh -s < tools/spi-loopback.sh | tee logs/spi-loopback.txt
#
# The question: when apple-z2 read back "eb 01 01 00 00", was that a real answer
# from the digitizer, or the SPI1 controller echoing our own TX (a loopback)?
# The header's checksum byte should have been 0x13 to make the five bytes sum to
# zero; instead it was 0x00 -- exactly what we sent.  So this drives a 4-byte PIO
# transfer of a known pattern with NO digitizer powered (hx-touchd not running,
# rails off, so MISO is not driven by the Z2) and reads RXDATA back.
#
#   RX == the TX pattern  -> the controller loops TX into RX for that CONFIG
#   RX == 00 / ff         -> no loopback; MISO simply idle
#
# It tries two CONFIG values: 0x10401e (what the driver uses now, REG_CONFIG_SET)
# and 0x4018 (the base XNU's AppleSamsungSPIController actually writes -- no bit
# 20, no bits 1,2).  If the first loops and the second does not, bit 20 is a
# self-test/loopback bit and the driver's CONFIG is the bug.

command -v peek >/dev/null 2>&1 || { echo "no peek on device"; exit 1; }
pgrep -f hx-touchd >/dev/null 2>&1 && { echo "hx-touchd is running -- stop it first (the driver owns the controller)"; exit 1; }

B=32100000
a() { printf '%x' $(( 0x$B + 0x$1 )); }
val() { peek r "$1" 1 | awk '{ print $2 }'; }

xfer() {                       # $1 = CONFIG base (hex, no 0x)
    cfg=$1
    peek w "$(a 0)"  d          >/dev/null   # CLKCFG = 0xd, run the clock
    peek w "$(a 8)"  0040000f   >/dev/null   # STATUS: clear + FIFO threshold
    peek w "$(a 30)" 4          >/dev/null   # CLKDIV = 4
    peek w "$(a c)"  6          >/dev/null   # PIN = 6
    peek w "$(a 4)"  "$cfg"     >/dev/null   # CONFIG idle
    peek w "$(a 34)" 4          >/dev/null   # RXCNT = 4
    peek w "$(a 4c)" 4          >/dev/null   # TXCNT = 4
    peek w "$(a 4)"  "$(printf '%x' $(( 0x$cfg | 0x20 )))" >/dev/null  # start, PIOEN
    for w in a5 5a c3 3c; do peek w "$(a 10)" "$w" >/dev/null; done    # push TX
    st=$(val "$(a 8)")                        # STATUS after the burst
    r0=$(val "$(a 20)"); r1=$(val "$(a 20)"); r2=$(val "$(a 20)"); r3=$(val "$(a 20)")
    peek w "$(a 4)"  "$cfg"     >/dev/null   # stop
    printf '   CONFIG=0x%s  TX=a5 5a c3 3c  ->  STATUS=%s  RX= %s %s %s %s\n' \
           "$cfg" "$st" "$r0" "$r1" "$r2" "$r3"
    txfifo=$(( (0x$st >> 6) & 0x1f )); rxfifo=$(( (0x$st >> 11) & 0x1f ))
    printf '                (STATUS says TX-FIFO level %d, RX-FIFO level %d)\n' "$txfifo" "$rxfifo"
}

echo "== SPI1 at rest (controller regs before we touch them)"
echo "   CLKCFG $(val "$(a 0)")  CONFIG $(val "$(a 4)")  STATUS $(val "$(a 8)")  CLKDIV $(val "$(a 30)")"
echo
echo "== transfer with the driver's current CONFIG (0x10401e)"
xfer 10401e
echo
echo "== transfer with XNU's base CONFIG (0x4018)"
xfer 4018
echo
echo "== leave the clock off so the next hx-touchd run re-inits cleanly"
peek w "$(a 0)" 0 >/dev/null
echo "   done"
