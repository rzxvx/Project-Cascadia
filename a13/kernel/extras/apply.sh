#!/bin/sh
# Add our kernel files to the tree (cwd = kernel source root, /extras = this dir).
set -eu
cp /extras/apple_fb_earlycon.c drivers/tty/serial/apple_fb_earlycon.c
if ! grep -q apple_fb_earlycon drivers/tty/serial/Makefile; then
    printf 'obj-$(CONFIG_SERIAL_EARLYCON) += apple_fb_earlycon.o\n' \
        >> drivers/tty/serial/Makefile
fi
echo "fb earlycon applied"
