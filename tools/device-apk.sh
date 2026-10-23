#!/bin/sh
# tools/device-apk.sh PKG... -- Alpine packages onto the iPad from this host,
# when the iPad has no network of its own (./cascadia net off, no Wi-Fi):
# fetched with their dependencies for armhf -- the iPad's userland, docker's
# linux/arm/v6 (the Mesa and piglit builds use armv7: the same ABI) --
# copied over ssh, and those not installed yet added with --allow-untrusted
# --no-network --force-broken-world (world names packages apk cannot reach
# offline).  Already installed dependencies are left as they are.
#
# What the GPU work (docs/research/p105-mesa.md) put on the iPad this way:
#   binutils sdl3-dbg musl-dbg         perf's annotate, symbols (M34)
#   waffle py3-mako py3-markupsafe     piglit (tools/piglit)
#   gdb                                backtraces (M36)
#   xwd xdotool                        X screenshots (tools/desktop/xshot.py)
#
# DEVICE=root@10.55.0.2 (the default); ALPINE=3.24.
set -eu
[ $# -gt 0 ] || { sed -n '2,20p' "$0"; exit 1; }
DEVICE="${DEVICE:-root@10.55.0.2}"
W=$(mktemp -d /tmp/cascadia-apk.XXXXXX)
trap 'rm -rf "$W"' EXIT
docker run --rm --platform linux/arm/v6 -v "$W":/out "alpine:${ALPINE:-3.24}" sh -c \
    "apk update >/dev/null && cd /out && apk fetch -R $*" >/dev/null
R=/tmp/cascadia-apk
ssh "$DEVICE" "rm -rf $R && mkdir -p $R"
for f in "$W"/*.apk; do
    ssh "$DEVICE" "cat > $R/$(basename "$f")" < "$f"
done
# shellcheck disable=SC2016
ssh "$DEVICE" 'cd '"$R"' && new= &&
    for f in *.apk; do
        n=$(echo "$f" | sed "s/-[0-9][^-]*-r[0-9]*\.apk$//")
        apk info -e "$n" >/dev/null 2>&1 || new="$new ./$f"
    done &&
    if [ -n "$new" ]; then apk add --allow-untrusted --no-network --force-broken-world $new; fi;
    rm -rf '"$R"
