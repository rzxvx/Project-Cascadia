#!/bin/sh
# touch-probe.sh - read-only look at the touch controller's surroundings on the
# phone (run as root). Changes nothing except binding spidev to the touch node.
# See docs/research/touch.md.

TS=/proc/device-tree/soc/spi@235104000/touchscreen@0

echo "== kernel log"
dmesg | grep -iE 'spi-apple-s5l|235104000|235018000|24a820000|i2c|pmgr.*(spi|sio|i2c)' | tail -20

echo "== calibration (m1n1 copies the ADT's multi-touch-calibration)"
if [ -f "$TS/apple,z2-cal-blob" ]; then
    n=$(wc -c < "$TS/apple,z2-cal-blob")
    echo "apple,z2-cal-blob: $n bytes"
    od -A x -t x1 -N 32 "$TS/apple,z2-cal-blob"
else
    echo "none"
fi

echo "== SMC keys named by the ADT (power_ldo rBK3, clock_enable gP00)"
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug
K=/sys/kernel/debug/apple_smc_keys/key
for k in gP00 rBK3; do
    echo "$k" > "$K" && cat "$K"
done

echo "== GPIOs: AP 95 (CS), AP 197 (reset), AOP 34 (interrupt)"
if command -v gpioinfo >/dev/null; then
    gpioinfo 2>/dev/null | grep -E '^gpiochip|line +(34|95|197):'
else
    echo "no gpioinfo (apk add libgpiod)"
fi

echo "== spidev on the touch node"
for d in /sys/bus/spi/devices/spi*; do
    [ "$(cat "$d/of_node/name" 2>/dev/null | tr -d '\0')" = touchscreen ] || continue
    dev=$(basename "$d")
    if [ ! -e "$d/driver" ]; then
        echo spidev > "$d/driver_override"
        echo "$dev" > /sys/bus/spi/drivers/spidev/bind
    fi
    echo "$dev -> $(basename "$(readlink "$d/driver")")"
done
ls -l /dev/spidev* 2>/dev/null

echo "== SPI controller parameters"
grep -H . /sys/module/spi_apple_s5l/parameters/* 2>/dev/null
