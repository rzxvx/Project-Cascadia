#!/bin/sh
# Native guest display diagnostic. Runtime files/cookie are private RAM only.
set -eu
export DISPLAY=:0
export XAUTHORITY=/run/k93-Xauthority
export HOME=/root
test "$(id -u)" = 0
grep -q 'apple,k93ap' /proc/device-tree/compatible
grep -q '^overlay / overlay ' /proc/mounts
test -c /dev/fb0
test ! -S /tmp/.X11-unix/X0
test ! -e /tmp/.X0-lock
test ! -e "$XAUTHORITY"
test -r /k93-xorg.conf
test -r /k93-twm.conf
test -x /k93-fb-read
echo K93_DESKTOP_BEGIN
umask 077
cookie=$(busybox timeout -k 1 5 /usr/bin/mcookie)
test "${#cookie}" = 32
case "$cookie" in *[!0123456789abcdefABCDEF]*) exit 1;; esac
touch "$XAUTHORITY"
/usr/bin/xauth -f "$XAUTHORITY" add :0 . "$cookie"
unset cookie
# Package triggers were intentionally not run when assembling the root.
/usr/bin/mkfontdir /usr/share/fonts/misc
test -s /usr/share/fonts/misc/fonts.dir
server_pid=
wm_pid=
term_pid=
cleanup() {
    trap - EXIT INT TERM
    for pid in "$term_pid" "$wm_pid" "$server_pid"; do
        if [ -n "$pid" ]; then kill "$pid" 2>/dev/null || :; fi
    done
    for pid in "$term_pid" "$wm_pid" "$server_pid"; do
        if [ -n "$pid" ]; then wait "$pid" 2>/dev/null || :; fi
    done
    echo K93_DESKTOP_CLEANUP
}
trap cleanup EXIT INT TERM
busybox timeout -k 2 35 /usr/libexec/Xorg :0 -config /k93-xorg.conf -auth "$XAUTHORITY" -nolisten tcp -noreset -logfile /run/k93-Xorg.log vt1 >/run/k93-Xorg-console.log 2>&1 &
server_pid=$!
count=0
while [ ! -S /tmp/.X11-unix/X0 ]; do
    kill -0 "$server_pid"
    count=$((count+1))
    test "$count" -le 10
    sleep 1
done
busybox timeout -k 2 30 /usr/bin/twm -f /k93-twm.conf >/run/k93-twm.log 2>&1 &
wm_pid=$!
busybox timeout -k 2 30 /usr/bin/xterm -title 'K93 native Alpine desktop' -geometry 100x38+35+60 -fn 9x15 -bg '#102030' -fg '#e5f2ff' -e /bin/sh -c 'printf "Native Linux on iPad 2\nAlpine ARMv7 framebuffer desktop\n\n"; uname -a; printf "\nTouch and persistence: under development\n"; exec /bin/sh' >/run/k93-xterm.log 2>&1 &
term_pid=$!
sleep 4
kill -0 "$server_pid"
kill -0 "$wm_pid"
kill -0 "$term_pid"
echo K93_DESKTOP_PROCESSES_ALIVE
/k93-fb-read >/run/k93-screen.ppm 2>/run/k93-fb-info.log
echo K93_FB_PPM_BEGIN
busybox gzip -c -1 /run/k93-screen.ppm | busybox base64
echo K93_FB_PPM_END
cat /run/k93-fb-info.log
busybox free -m
sleep 5
echo K93_DESKTOP_END
