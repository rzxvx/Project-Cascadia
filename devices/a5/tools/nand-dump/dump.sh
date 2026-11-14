#!/bin/sh
# Full raw dump of the NAND in the w0c layout, on the device:
#   dump.sh <output dir>
# Needs apple_ppn and ppn_dump loaded. Reads only.
set -e
out=$1
[ -n "$out" ] || { echo "usage: $0 <dir>"; exit 2; }
mkdir -p "$out"
for bus in 0 1; do
  for cau in 0 1; do
    for seg in 0 500 1000; do
      n=500; [ $seg = 1000 ] && n=64
      f="$out/bus$bus-cau$cau.b$seg.bin"
      echo "$bus $cau $seg $n" > /dev/ppndump
      start=$(date +%s)
      cat /dev/ppndump > "$f"
      cat /dev/ppndump_st > "$f.st"
      echo "$(date +%T) bus $bus cau $cau blocks $seg+$n: $(( $(date +%s) - start )) s" | tee -a "$out/dump.log"
    done
  done
done
echo done | tee -a "$out/dump.log"
