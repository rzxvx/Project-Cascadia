#!/bin/sh
# isa-run.sh NAME [probe args]: run build/gpu/NAME.bin's programs (the
# names and expected words in build/gpu/NAME.txt) through g9p-probe on the
# iPad (HOST); prints want / got per program and a summary.
set -e
B="$(cd "$(dirname "$0")/../../../build/gpu" && pwd)"
HOST=${HOST:-root@172.16.43.1}
N=$1; shift
scp -q "$B/$N.bin" "$HOST:/run/$N.bin"
ok=0; bad=0
while read -r name want; do
    got=$(ssh -n "$HOST" "/run/g9p-probe $name file=/run/$N.bin tail=1 dump=1 $*" 2>&1 |
          sed -n 's/.*buf 0 \[   0\] = //p')
    if [ "$got" = "$want" ]; then ok=$((ok + 1)); r=ok; else bad=$((bad + 1)); r=FAIL; fi
    printf '%-14s want %s got %s %s\n' "$name" "$want" "${got:-none}" "$r"
done < "$B/$N.txt"
echo "$N: $ok ok, $bad failed"
[ "$bad" = 0 ]
