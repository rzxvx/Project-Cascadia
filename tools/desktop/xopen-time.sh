#!/bin/sh
# tools/desktop/xopen-time.sh [N] -- on the iPad, in a running X session: a
# terminal opened N times (6 by default), each until its window shows; the
# times, their mean leaving out the first (files still cold), and the X
# server's CPU a window (docs/research/p105-mesa.md, M45).  The yardstick
# of what the X server costs: start the session with the build to measure
# (an ~/.xserverrc of `exec env SGX_MESA=DIR /root/sgx-dev /usr/libexec/Xorg
# "$@"`), wait a minute for it to settle, then run this.
N=${1:-6}
export DISPLAY=${DISPLAY:-:0} HOME=${HOME:-/root}
x=$(pidof Xorg) || { echo "xopen-time: no X server" >&2; exit 1; }
# the session's bus, as a terminal from the panel has it (from ssh there is
# none: each terminal would start dbus-launch, and xfconf-query its own
# xfconfd, which writes the session's settings files behind its back)
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] && s=$(pidof xfce4-session); then
    eval "export $(tr '\0' '\n' < /proc/${s%% *}/environ | grep '^DBUS_SESSION_BUS_ADDRESS=')"
fi
c0=$(awk '{print $14 + $15}' /proc/$x/stat)
i=0; tot=0
while [ $i -lt $N ]; do
    read t0 _ < /proc/uptime
    xfce4-terminal --disable-server -T xopen-time >/dev/null 2>&1 &
    xdotool search --sync --onlyvisible --name xopen-time >/dev/null
    read t1 _ < /proc/uptime
    # (uptime in hundredths: the dot taken out)
    ms=$(( (${t1%.*}${t1#*.} - ${t0%.*}${t0#*.}) * 10 ))
    printf "%d " $ms
    [ $i -gt 0 ] && tot=$((tot + ms))
    sleep 1
    for w in $(xdotool search --name xopen-time); do xdotool windowkill $w; done
    sleep 2
    i=$((i + 1))
done
c1=$(awk '{print $14 + $15}' /proc/$x/stat)
[ $N -gt 1 ] && echo "ms; mean $((tot / (N - 1))) ms but the first; Xorg's CPU $(( (c1 - c0) * 10 / N )) ms a window"
