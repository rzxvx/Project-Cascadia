#!/bin/sh
# hangs.sh now | SINCE -- on the iPad: the renders the kernel gave up on
# ("not done after") since SINCE, a dmesg timestamp -- "now" prints the
# current one.  Counting all of dmesg misleads: its ring buffer rolls, and
# the kernel's message comes about 2 s after the render, so sleep 3 first.
#   T=$(hangs.sh now); sgx-gl glfs; sleep 3; hangs.sh $T
if [ "$1" = now ]; then awk '{print $1}' /proc/uptime; exit; fi
dmesg | grep "not done after" | awk -F"[][]" -v t="$1" '$2+0 > t+0' | wc -l
