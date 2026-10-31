#!/bin/sh
# Add our kernel files to the tree (cwd = kernel source root, /extras = this dir).
set -eu
cp /extras/apple_fb_earlycon.c drivers/tty/serial/apple_fb_earlycon.c
if ! grep -q apple_fb_earlycon drivers/tty/serial/Makefile; then
    printf 'obj-$(CONFIG_SERIAL_EARLYCON) += apple_fb_earlycon.o\n' \
        >> drivers/tty/serial/Makefile
fi
# Touch bring-up: the "spi-1,samsung" SPI controller, raw SMC keys in debugfs
cp /extras/spi-apple-s5l.c drivers/spi/spi-apple-s5l.c
if ! grep -q spi-apple-s5l drivers/spi/Makefile; then
    printf 'obj-$(CONFIG_SPI_MASTER) += spi-apple-s5l.o\n' >> drivers/spi/Makefile
fi
cp /extras/apple_smc_keys.c drivers/mfd/apple_smc_keys.c
if ! grep -q apple_smc_keys drivers/mfd/Makefile; then
    printf 'obj-$(CONFIG_MFD_MACSMC) += apple_smc_keys.o\n' >> drivers/mfd/Makefile
fi
# The touchscreen (HBPP firmware load + HID-over-SPI)
cp /extras/apple_hidspi_touch.c drivers/input/touchscreen/apple_hidspi_touch.c
if ! grep -q apple_hidspi_touch drivers/input/touchscreen/Makefile; then
    printf 'obj-$(CONFIG_SPI_MASTER) += apple_hidspi_touch.o\n' >> drivers/input/touchscreen/Makefile
fi
echo "extras applied"
