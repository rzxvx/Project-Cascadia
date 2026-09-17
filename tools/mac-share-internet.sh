#!/usr/bin/env bash
#
# Give the iPad a route to the internet over the CDC ECM link, so apk on the
# device can reach the Alpine mirrors.  Runs on macOS.
#
#   ./tools/mac-share-internet.sh on
#   ./tools/mac-share-internet.sh off
#   ./tools/mac-share-internet.sh status
#
# Why pf by hand rather than System Settings > Internet Sharing: Internet
# Sharing re-addresses the interface and runs its own DHCP on 192.168.2.0/24,
# which would throw away the pinned 10.55.0.2 that ssh and every note in these
# docs depend on.  This keeps the addressing and adds only NAT.
#
# The rules load into a CHILD ANCHOR under com.apple/, which macOS's stock
# /etc/pf.conf already references with nat-anchor "com.apple/*" and anchor
# "com.apple/*".  So they take effect without editing a system file and without
# flushing the main ruleset -- which matters because flushing it would take out
# whatever your VPN and other services have installed there.  Likewise `pfctl
# -E` refcounts pf up and hands back a token; `pfctl -d` would disable pf for
# everyone, so this never calls it.
set -euo pipefail

ANCHOR="com.apple/cascadia"
RULES="/etc/pf.anchors/cascadia"
TOKENFILE="${TMPDIR:-/tmp}/cascadia-pf.token"
LAN="${LAN:-10.55.0.0/24}"

[ "$(uname -s)" = "Darwin" ] || { echo "this one runs on the Mac, not the device" >&2; exit 1; }

# Downstream: the gadget.  Its hardware port name is g_cdc's own DRIVER_DESC,
# which is why it is worth matching on rather than on an interface number.
find_down() {
    networksetup -listallhardwareports \
        | awk '/Hardware Port: CDC Composite Gadget/{getline; print $2; exit}'
}
# Upstream: whatever currently carries the default route.
find_up() { route -n get default 2>/dev/null | awk '/interface:/{print $2; exit}'; }

case "${1:-}" in
on)
    DOWN="${DOWN:-$(find_down)}"
    UP="${UP:-$(find_up)}"
    [ -n "$DOWN" ] || { echo "no 'CDC Composite Gadget' interface -- is the iPad booted and plugged in?" >&2; exit 1; }
    [ -n "$UP" ]   || { echo "no default route -- this Mac is not online" >&2; exit 1; }
    [ "$DOWN" != "$UP" ] || { echo "upstream and downstream are both $UP; refusing" >&2; exit 1; }
    echo "==> upstream $UP, downstream $DOWN, clients $LAN"

    sudo tee "$RULES" >/dev/null <<EOF
nat on $UP inet from $LAN to any -> ($UP)
pass in quick on $DOWN inet from $LAN to any keep state
pass out quick on $UP inet from $LAN to any keep state
EOF
    sudo pfctl -vnf "$RULES" >/dev/null       # syntax check before loading
    sudo sysctl -w net.inet.ip.forwarding=1 >/dev/null
    sudo pfctl -a "$ANCHOR" -f "$RULES"
    sudo pfctl -E 2>&1 | awk '/Token/{print $3}' > "$TOKENFILE"
    echo "==> on.  Token $(cat "$TOKENFILE") saved to $TOKENFILE"
    echo "    test from the device:  ping -c3 1.1.1.1  &&  apk update"
    ;;
off)
    sudo pfctl -a "$ANCHOR" -F all 2>/dev/null || true
    if [ -s "$TOKENFILE" ]; then
        # Release OUR reference only.  pfctl -d would disable pf globally, and
        # other macOS services are holding their own references to it.
        sudo pfctl -X "$(cat "$TOKENFILE")" 2>/dev/null || true
        rm -f "$TOKENFILE"
    fi
    sudo sysctl -w net.inet.ip.forwarding=0 >/dev/null
    echo "==> off (pf itself left enabled for whoever else is using it)"
    ;;
status)
    echo "forwarding: $(sysctl -n net.inet.ip.forwarding)"
    echo "downstream: $(find_down || true)    upstream: $(find_up || true)"
    echo "--- anchor $ANCHOR ---"
    sudo pfctl -a "$ANCHOR" -s nat 2>/dev/null || echo "(empty)"
    sudo pfctl -a "$ANCHOR" -s rules 2>/dev/null || true
    ;;
*)
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
