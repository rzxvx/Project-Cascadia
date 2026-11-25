#!/bin/sh
# isa-run.sh NAME [probe args]: run build/gpu/NAME.bin's programs through
# g9p-probe on the iPad (HOST). build/gpu/NAME.txt: one line per program,
# "name want0 [want1 ...]" -- the expected words 0, 1, ... of probe buffer 0
# (hex); prints the result per program and a summary.
set -e
B="$(cd "$(dirname "$0")/../../../build/gpu" && pwd)"
HOST=${HOST:-root@172.16.43.1}
N=$1; shift
scp -q "$B/$N.bin" "$HOST:/run/$N.bin"
ok=0; bad=0
while read -r name wants; do
    n=$(echo $wants | wc -w)
    got=$(ssh -n "$HOST" "/run/g9p-probe $name file=/run/$N.bin tail=1 dump=4096 $*" 2>&1 |
          awk -v n="$n" '/buf 0 \[/ { gsub(/[\[\]]/, ""); i = $3 + 0; if (i < n) v[i] = $5 }
                         END { s = ""; for (i = 0; i < n; i++) s = s (i ? " " : "") (i in v ? v[i] : "-"); print s }')
    if [ "$got" = "$wants" ]; then ok=$((ok + 1)); r=ok; else bad=$((bad + 1)); r=FAIL; fi
    if [ "$r" = ok ] || [ "$n" -le 4 ]; then
        printf '%-16s %s  (%s)\n' "$name" "$r" "$got"
    else
        printf '%-16s %s\n   want %s\n   got  %s\n' "$name" "$r" "$wants" "$got"
    fi
done < "$B/$N.txt"
echo "$N: $ok ok, $bad failed"
[ "$bad" = 0 ]
