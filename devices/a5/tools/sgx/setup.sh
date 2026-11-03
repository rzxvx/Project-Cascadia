#!/bin/sh
# setup.sh -- after a reboot: debugfs, fbcon off the screen, microkernel up, the
# GPU mappings the transfer tests use, and GPU VA 0x9e000000 (96 pages) pointing
# at framebuffer rows 300..427 (simplefb at PA 0x9f6fc000, 3072-byte lines).
D=/sys/kernel/debug/apple-sgx
mount -t debugfs none /sys/kernel/debug 2>/dev/null
echo 0 > /sys/class/vtconsole/vtcon1/bind
dmesg -n 1
echo 1 > $D/boot; sleep 0.2
for m in "0x98104000 0x6000" "0x1000 0x7000" "0x980f3000 0x10000" "0x8c000000 0x10000" \
         "0x9d000000 0x60000" "0x9e000000 0x60000"; do echo "map $m" > $D/cmd; done
pd=0x$(grep "page directory pa" $D/regs | awk '{print $NF}' | sed 's/^0x//')
pde=$(peek r $(printf %x $((pd + (0x9e000000>>22)*4))) 1 | awk '{print $2}')
pt=$(( 0x$pde & 0xfffff000 ))
i=0; while [ $i -lt 96 ]; do peek w $(printf %x $((pt + i*4))) $(printf %x $((0x9f7dd001 + i*0x1000))) >/dev/null; i=$((i+1)); done
echo "setup: $(dmesg | grep -c 'mapped GPU') mappings, fb window PT $(printf %x $pt)"
