#!/bin/sh
# SPDX-License-Identifier: MIT
# g9p-suite.sh [HOST]: the g9p driver's pixel checks on the iPad (g9p-glcheck
# and g9p-gltri from /usr/local/g9p/bin, mesa/install.sh puts them there);
# one line per check, the failures counted at the end.
HOST=${1:-${HOST:-root@172.16.43.1}}
ssh "$HOST" 'cd /usr/local/g9p/bin || exit 1
pass=0 fail=0
run() {
  out=$("$@" 2>&1 | grep -v "^MESA: info")
  if echo "$out" | grep -q "^ok" && ! echo "$out" | grep -q "FAIL\|rror"; then
    pass=$((pass + 1)); echo "ok    $* :: $(echo "$out" | grep "^ok" | tail -1)"
  else
    fail=$((fail + 1)); echo "FAIL  $*"; echo "$out" | head -4
  fi
}
C="./g9p-run ./g9p-glcheck"
for s in 1 64 333 1620; do run ./g9p-run ./g9p-gltri $s $s 1; done
for m in arrays elements user8 strip fan; do run ./g9p-run ./g9p-gltri 64 64 1 $m; done
for m in uniform uniform-vs uniform-fs uniform-vs4 uniform-vs4hi uniform-vs4far; do run $C $m; done
run $C tex 16 16; run $C tex 37 5 200 100; run $C texmod 32 32
for f in less lequal greater gequal equal notequal always never nowrite; do
  run $C depth $f; run env G9P_CHECK_SPLIT=1 $C depth $f; run env G9P_CHECK_Z16=1 $C depth $f
done
for f in back front none frontcw; do run $C cull $f; done
run $C stencil; run env G9P_CHECK_SPLIT=1 $C stencil
run $C discard; run $C discard 1000 700; run $C discardblend; run $C scissor; run $C scissor 300 200
for f in ubyte short byte odd; do run $C vfmt $f; done
run $C load; run $C load 200 100 4
for a in 0,1000 0,1000,0.05,0.1 0,-300,0.6,0.9 3,0,0.05,0.9 -2,0,0.2,0.9 2,200 0,4 0,-4 \
         0,1000,0.3,0.7,0.00003 4,0,0.3,0.7,0.003 -4,-10,0.3,0.7,-0.002; do
  run $C polyoffset $a; run env G9P_CHECK_SPLIT=1 $C polyoffset $a
done
for a in 0,1000 2,200 0,-4; do run env G9P_CHECK_Z16=1 $C polyoffset $a; done
run $C polyoffset 2,100 1620 1000
for a in none,0 bias,1 bias,2.2 bias,-1 bias,9 bias,1,1 bias,-0.7,1.9 bias,1.25,0,l none,0,1.75,l \
         level,0 level,3.3 level,8 level,1,2 level,0.75,0,l level,3,-1.5,l; do
  run $C lod $a
done
run $C lod none,0 32 32; run $C lod bias,1 32 32; run $C lod none,0,1.2 16 16
echo "== $pass passed, $fail failed"
[ "$fail" = 0 ]'
