# Runs ON THE iPad, fed over ssh:
#
#     ssh root@10.55.0.2 sh -s < tools/spi-pinmux.sh | tee logs/spi-pinmux.txt
#
# MISO reads a clean 0x00 for every byte of every transfer -- never 0xff, never
# noise -- which is what a pin that is not actually switched to the SPI
# peripheral looks like.  The dts gives spi1 no pinctrl at all; we have been
# assuming iBoot left the pads muxed.
#
# Dump every GPIO pin's config register and decode it:
#   0x3fa00000 + pin*4
#     bit0      DATA / input level
#     bits1..3  mode (1 = output, 5 = falling-edge irq, 7 = off)
#     bit5      PERIPH  <- set means the pad belongs to a peripheral, not GPIO
#     bit9      CFG_DONE
#
# CS is GPIO 103 (ADT 0x0c07 = port 12 pin 7), so spi1's clock and data pads
# should be its neighbours in port 12 (pins 96..103).

command -v peek >/dev/null 2>&1 || { echo "no peek on device"; exit 1; }

peek r 3fa00000 224 | awk '
function hex(s,   i,c,v,d) {
    v = 0
    s = tolower(s)
    for (i = 1; i <= length(s); i++) {
        c = substr(s, i, 1)
        d = index("0123456789abcdef", c) - 1
        if (d < 0) continue
        v = v * 16 + d
    }
    return v
}
{
    base = hex(substr($1, 1, length($1) - 1))
    for (i = 2; i <= NF; i++) {
        addr = base + (i - 2) * 4
        pin  = int((addr - hex("3fa00000")) / 4)
        v    = hex($i)
        mode = int(v / 2) % 8
        per  = int(v / 32) % 2
        dat  = v % 2
        cfg  = int(v / 512) % 2
        port = int(pin / 8); bit = pin % 8
        if (per)
            printf "  PERIPH pin %3d (port %2d pin %d, ADT 0x%02x%02x)  reg=%08x  mode=%d data=%d cfg=%d\n", pin, port, bit, port, bit, v, mode, dat, cfg
        if (pin >= 96 && pin <= 111)
            p12[pin] = sprintf("  port12 pin %3d (ADT 0x%02x%02x)  reg=%08x  mode=%d periph=%d data=%d", pin, port, bit, v, mode, per, dat)
    }
}
END {
    print ""
    print "== port 12 neighbourhood (CS is pin 103):"
    for (k = 96; k <= 111; k++) if (k in p12) print p12[k]
}'
