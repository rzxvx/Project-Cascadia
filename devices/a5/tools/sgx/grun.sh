#!/bin/sh
# grun.sh DIR [SRC] -- run a generated transfer (g_code/g_block/g_param/g_cmd from DIR)
D=/sys/kernel/debug/apple-sgx
W() { dd if=$1 of=$D/mem bs=4096 seek=$(($2)) conv=notrunc 2>/dev/null; }
dd if=/dev/zero bs=4096 count=96 2>/dev/null > /tmp/z.bin; W /tmp/z.bin 0x9d000
dd if=/dev/zero bs=4096 count=7 2>/dev/null > /tmp/z7.bin; W /tmp/z7.bin 0x1
W $1/g_code.bin 0x1; W $1/g_block.bin 0x980f3; W ${2:-/tmp/src_pat.bin} 0x98104; W $1/g_param.bin 0x8c006
echo 1 > $D/boot; W $1/g_cmd.bin 0x80146; echo tqkick > $D/cmd 2>/dev/null; sleep 0.3
f=$(grep -E "BIF_FAULT " $D/regs | awk '{print $NF}' | grep -v 0x00000000 | tr '\n' ' ')
dd if=$D/mem bs=4096 skip=$((0x9d000)) count=96 2>/dev/null > /tmp/lout.bin
echo "read $(grep 'tq CCB' $D/regs | awk '{print $6}') faults ${f:-none} lockups $(grep lockups $D/regs | awk '{print $NF}') md5 $(md5sum /tmp/lout.bin | cut -c1-8)"
