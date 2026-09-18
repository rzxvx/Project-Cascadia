#!/usr/bin/env bash
#
# Bring up this side of the USB link: give the gadget's network interface
# 10.55.0.1/24, then wait until the device answers on 10.55.0.2.
#
#   ./cascadia link                  wait up to 180 s, configure, ping
#   LINK_WAIT=600 ./cascadia link
#
# ./cascadia flash runs this by itself once the loader is sent; it is its own
# command for the times the device was booted some other way, or rebooted.
#
# Why it cannot be a one-time setting: the address is not remembered.  The
# interface is created when the device enumerates and destroyed when it goes
# away -- and one boot makes it go away twice, when iBEC hands over and again
# when /init forces a clean re-enumeration -- so an address set by hand is
# usually gone before the boot even finishes.  Without it, packets for
# 10.55.0.2 follow the default route out to the internet and ssh just hangs,
# with no error to say why.
#
# Timing matters for more than ssh.  Stage 1 mounts the NFS root from
# 10.55.0.1 right after it brings usb0 up, and mount.nfs keeps retrying for
# about two minutes before stage 1 gives up and stays on the RAM root.  So this
# has to run while the device boots, not after -- which is why flash calls it
# straight after the upload rather than leaving it as a step to remember.
#
# The interface is found by the MAC pinned in the bootargs (g_cdc.host_addr),
# which is the same on macOS and on Linux, rather than by a name only macOS
# knows.  The Linux branch has not met real hardware yet.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IP_HOST="${IP_HOST:-10.55.0.1}"
IP_DEV="${IP_DEV:-10.55.0.2}"
LINK_WAIT="${LINK_WAIT:-180}"

# One source of truth for the MAC: the bootargs the device actually boots with.
MAC=$(LC_ALL=C grep -o 'g_cdc.host_addr=[0-9A-Fa-f:]*' "$ROOT/dts/p105ap.dts" 2>/dev/null \
      | head -1 | cut -d= -f2 | tr 'A-F' 'a-f' || true)
MAC="${MAC:-02:10:5a:05:00:01}"

OS=$(uname -s)
case "$OS" in
    Darwin|Linux) ;;
    *) echo "error: no host-side link setup for $OS" >&2; exit 1 ;;
esac

find_if() {
    local i=""
    if [ "$OS" = Darwin ]; then
        i=$(ifconfig -a 2>/dev/null | awk -v mac="$MAC" '
            /^[a-z]/                             { iface = $1; sub(/:$/, "", iface) }
            $1 == "ether" && tolower($2) == mac  { print iface; exit }')
        # Fallback: the hardware port macOS names after g_cdc's DRIVER_DESC,
        # which is what mac-share-internet.sh matches on.
        if [ -z "$i" ]; then
            i=$(networksetup -listallhardwareports 2>/dev/null \
                | awk '/Hardware Port: CDC Composite Gadget/{getline; print $2; exit}')
        fi
        if [ -n "$i" ]; then echo "$i"; fi
    else
        for n in /sys/class/net/*; do
            if [ "$(cat "$n/address" 2>/dev/null)" = "$MAC" ]; then
                basename "$n"; return 0
            fi
        done
    fi
    return 0
}

has_ip() {
    if [ "$OS" = Darwin ]; then
        ifconfig "$1" 2>/dev/null | grep -q "inet $IP_HOST "
    else
        ip -4 addr show dev "$1" 2>/dev/null | grep -q "inet $IP_HOST/"
    fi
}

assign() {
    if [ "$OS" = Darwin ]; then
        sudo ifconfig "$1" "$IP_HOST" netmask 255.255.255.0 up
    else
        sudo ip addr replace "$IP_HOST/24" dev "$1"
        sudo ip link set "$1" up
    fi
}

answers() {
    if [ "$OS" = Darwin ]; then
        ping -c 1 -t 1 "$IP_DEV" >/dev/null 2>&1
    else
        ping -c 1 -W 1 "$IP_DEV" >/dev/null 2>&1
    fi
}

echo "==> link: waiting up to ${LINK_WAIT}s for the device (host side MAC $MAC)"
deadline=$(( $(date +%s) + LINK_WAIT ))
last=""
while [ "$(date +%s)" -lt "$deadline" ]; do
    IF=$(find_if)
    if [ -n "$IF" ]; then
        if [ "$IF" != "$last" ]; then echo "    interface $IF is here"; last="$IF"; fi
        # Checked on every pass, not once: each re-enumeration recreates the
        # interface, and the address goes with it.
        # Not fatal: the interface can vanish between finding it and
        # configuring it, which is exactly the re-enumeration being waited out.
        if ! has_ip "$IF"; then
            if assign "$IF"; then echo "    $IF = $IP_HOST/24"; fi
        fi
        if answers; then
            echo "==> link: $IP_DEV answers"
            echo "    ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@$IP_DEV"
            exit 0
        fi
    fi
    sleep 1
done

if [ -z "$last" ]; then
    echo "error: no interface with MAC $MAC appeared in ${LINK_WAIT}s --" >&2
    echo "       the gadget never enumerated as a network device." >&2
else
    echo "error: $last has $IP_HOST, but $IP_DEV never answered." >&2
fi
echo "       The ACM console needs no address at all:" >&2
echo "         ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null;  screen <that> 115200" >&2
echo "       and from there:  ip addr show usb0;  dmesg | grep P105:" >&2
exit 1
