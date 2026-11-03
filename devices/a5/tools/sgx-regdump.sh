#!/bin/sh
# sgx-regdump.sh NAME -- every word of the SGX543MP2's master, core 0 and
# core 1 register banks to /tmp/NAME.txt (peek r format), for diffing the
# state before and after a microkernel command.  Run on the device, with the
# GPU booted (echo 1 > /sys/kernel/debug/apple-sgx/boot): with its clocks off
# the first read hangs the bus.  Every offset of the three banks reads back
# safely (2026-10-01).  The master's registers survive a microkernel boot, the
# cores' do not.
out=/tmp/${1:?usage: sgx-regdump.sh NAME}.txt
: > "$out"
for base in 35104000 35108000 3510c000; do
	for o in 0 400 800 c00 1000 1400 1800 1c00 2000 2400 2800 2c00 3000 3400 3800 3c00; do
		peek r "$(printf %x $((0x$base + 0x$o)))" 0x100 >> "$out"
	done
done
wc -l "$out"
