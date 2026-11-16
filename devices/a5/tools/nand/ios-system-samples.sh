#!/usr/bin/env bash
set -uo pipefail
ROOT=$HOME/Desktop/cascadia-main/devices/a5
fail() { echo "error: $*" >&2; exit 1; }
. "$ROOT/tools/ios-ssh.sh"
ios_setup samples
ios_check_model >/dev/null
for i in $(seq ${START:-0} $(( ${END:-64} - 1 ))); do
	n=$(( (i * 8513 + 17) % 543000 ))
	h=$(ios "dd if=/dev/rdisk0s1s1 bs=4096 skip=$n count=1 2>/dev/null | openssl sha1" </dev/null 2>&1 | sed 's/.*= //')
	echo "$n $h"
done
h=$(ios "dd if=/dev/rdisk0s1s1 bs=1048576 count=16 2>/dev/null | openssl sha1" </dev/null 2>&1 | sed 's/.*= //')
echo "first16MiB $h"
