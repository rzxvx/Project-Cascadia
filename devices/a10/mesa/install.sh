#!/bin/sh
# install.sh [HOST]: build/mesa/g9p-mesa.tar.gz (build.sh driver) onto the
# iPad at /usr/local/g9p, beside the system's Mesa (which stays the default:
# g9p-run picks this one; linked into /usr/local/bin, which is on PATH).
set -e
HOST=${1:-root@172.16.43.1}
T="$(cd "$(dirname "$0")/.." && pwd)/build/mesa/g9p-mesa.tar.gz"
scp -q "$T" "$HOST:/tmp/g9p-mesa.tar.gz"
ssh "$HOST" 'set -e; cd /tmp && rm -rf g9p && tar xzf g9p-mesa.tar.gz &&
  mkdir -p /usr/local/g9p/bin && for f in /usr/local/g9p/bin/*; do
    [ -e "/tmp/g9p/bin/${f##*/}" ] || cp "$f" /tmp/g9p/bin/; done &&
  rm -rf /usr/local/g9p && mv /tmp/g9p /usr/local/g9p && rm /tmp/g9p-mesa.tar.gz &&
  ln -sf /usr/local/g9p/bin/g9p-run /usr/local/bin/g9p-run'
echo "==> $HOST:/usr/local/g9p"
