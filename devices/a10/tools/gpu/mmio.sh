#!/bin/sh
# Build tools/gpu/mmio.c and mmiod.c as static aarch64 binaries (build/bin/)
# in an arm64 Alpine container. Copy them to the iPad's /run with scp.
set -e
A10="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$A10/build/bin"
docker run --rm --platform linux/arm64 -v "$A10:/a10" alpine:3.24 sh -c \
  'apk add --no-cache gcc musl-dev >/dev/null &&
   for t in mmio mmiod; do gcc -O2 -static -Wall -o /a10/build/bin/$t /a10/tools/gpu/$t.c || exit 1; done'
file "$A10/build/bin/mmio" "$A10/build/bin/mmiod"
