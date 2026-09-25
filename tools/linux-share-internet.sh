#!/usr/bin/env bash
#
# Give the iPad a route to the internet over the CDC ECM link, so apk on the
# device can reach the Alpine mirrors.  The Linux twin of mac-share-internet.sh;
# ./cascadia net picks by host.
#
#   ./tools/linux-share-internet.sh on
#   ./tools/linux-share-internet.sh off
#   ./tools/linux-share-internet.sh status
#
# Forwarding plus masquerade, and nothing else: the pinned 10.55.0.1/10.55.0.2
# stay as they are.  The rules go in through iptables when it is there, and it
# is on any host that runs docker, which this build wants: docker sets the
# FORWARD policy to DROP, and an accept in a table of our own would not get a
# packet past a drop in docker's -- every base chain on a hook has to let it
# through.  So the accepts go into FORWARD itself, first in line.  Each rule
# carries the comment "cascadia", and `off` deletes exactly those.  Without
# iptables, nftables gets a table of its own, which is enough where nothing
# else drops forwarded traffic.
set -euo pipefail

LAN="${LAN:-10.55.0.0/24}"
TAG=cascadia
STATE="${XDG_RUNTIME_DIR:-/tmp}/cascadia-ip_forward"   # the value `on` found, for `off`

[ "$(uname -s)" = "Linux" ] || { echo "this one runs on a Linux host; on macOS it is mac-share-internet.sh" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Downstream: the gadget, found by the MAC pinned in the bootargs, as
# host-link.sh does -- its name on Linux depends on the USB port it sits on.
find_down() {
    local mac
    mac=$(LC_ALL=C grep -o 'g_cdc.host_addr=[0-9A-Fa-f:]*' "$ROOT/dts/p105ap.dts" 2>/dev/null \
          | head -1 | cut -d= -f2 | tr 'A-F' 'a-f' || true)
    mac="${mac:-02:10:5a:05:00:01}"
    for n in /sys/class/net/*; do
        if [ "$(cat "$n/address" 2>/dev/null)" = "$mac" ]; then basename "$n"; return 0; fi
    done
}
# Upstream: whatever currently carries the default route.
find_up() { ip -4 route show default 2>/dev/null | awk '{ for (i = 1; i < NF; i++) if ($i == "dev") { print $(i + 1); exit } }'; }

# Our iptables rules, as delete commands: every line of -S that carries the tag,
# with -A turned into -D.
ipt_ours() { sudo iptables -t "$1" -S 2>/dev/null | grep -F -- "--comment $TAG" | sed 's/^-A /-D /' || true; }

case "${1:-}" in
on)
    DOWN="${DOWN:-$(find_down)}"
    UP="${UP:-$(find_up)}"
    [ -n "$DOWN" ] || { echo "no interface with the gadget's MAC -- is the iPad booted and plugged in?" >&2; exit 1; }
    [ -n "$UP" ]   || { echo "no default route -- this host is not online" >&2; exit 1; }
    [ "$DOWN" != "$UP" ] || { echo "upstream and downstream are both $UP; refusing" >&2; exit 1; }
    echo "==> upstream $UP, downstream $DOWN, clients $LAN"

    [ -s "$STATE" ] || cat /proc/sys/net/ipv4/ip_forward > "$STATE"
    sudo sysctl -qw net.ipv4.ip_forward=1
    if have iptables; then
        # Idempotent: a second `on` replaces the rules rather than stacking them.
        ipt_ours nat    | while read -r r; do eval "sudo iptables -t nat $r"; done
        ipt_ours filter | while read -r r; do eval "sudo iptables -t filter $r"; done
        sudo iptables -t nat -A POSTROUTING -s "$LAN" -o "$UP" -m comment --comment "$TAG" -j MASQUERADE
        sudo iptables -I FORWARD 1 -i "$UP" -o "$DOWN" -d "$LAN" \
            -m conntrack --ctstate RELATED,ESTABLISHED -m comment --comment "$TAG" -j ACCEPT
        sudo iptables -I FORWARD 1 -i "$DOWN" -o "$UP" -s "$LAN" -m comment --comment "$TAG" -j ACCEPT
        echo "==> on (iptables, rules tagged '$TAG')"
    elif have nft; then
        sudo nft -f - <<EOF
table ip $TAG
delete table ip $TAG
table ip $TAG {
    chain postrouting {
        type nat hook postrouting priority srcnat;
        ip saddr $LAN oifname "$UP" masquerade
    }
    chain forward {
        type filter hook forward priority filter;
        iifname "$DOWN" oifname "$UP" ip saddr $LAN accept
        iifname "$UP" oifname "$DOWN" ip daddr $LAN ct state established,related accept
    }
}
EOF
        echo "==> on (nftables, table ip $TAG)"
    else
        echo "neither iptables nor nft here (Arch: sudo pacman -S iptables-nft)" >&2; exit 1
    fi
    # ufw by its own switch, not systemctl: ufw.service stays "active" with the
    # firewall disabled (linux-nfs-export.sh has the same check).
    fw=""
    if systemctl is-active --quiet firewalld 2>/dev/null; then fw=firewalld; fi
    if grep -qs '^ENABLED=yes' /etc/ufw/ufw.conf; then fw=ufw; fi
    if [ -n "$fw" ]; then
        echo "note: $fw is enabled and may still drop forwarded traffic; if ping fails, allow $DOWN -> $UP there."
    fi
    echo "    test from the device:  ping -c3 1.1.1.1  &&  apk update"
    ;;
off)
    if have iptables; then
        ipt_ours nat    | while read -r r; do eval "sudo iptables -t nat $r"; done
        ipt_ours filter | while read -r r; do eval "sudo iptables -t filter $r"; done
    fi
    if have nft; then sudo nft delete table ip "$TAG" 2>/dev/null || true; fi
    # Forwarding goes back to what `on` found: docker, for one, wants it on.
    if [ -s "$STATE" ]; then
        sudo sysctl -qw net.ipv4.ip_forward="$(cat "$STATE")"
        rm -f "$STATE"
    fi
    echo "==> off (ip_forward = $(cat /proc/sys/net/ipv4/ip_forward))"
    ;;
status)
    echo "ip_forward: $(cat /proc/sys/net/ipv4/ip_forward)"
    echo "downstream: $(find_down || true)    upstream: $(find_up || true)"
    if have iptables; then
        echo "--- iptables rules tagged '$TAG' ---"
        { sudo iptables -t nat -S; sudo iptables -t filter -S; } 2>/dev/null | grep -F -- "--comment $TAG" || echo "(none)"
    fi
    if have nft; then
        echo "--- nft table ip $TAG ---"
        sudo nft list table ip "$TAG" 2>/dev/null || echo "(none)"
    fi
    ;;
*)
    sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
    ;;
esac
