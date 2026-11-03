#!/bin/sh
# Bounded, offline, unprivileged native browser diagnostic. No sandbox bypass.
set -eu
export DISPLAY=:0 XAUTHORITY=/run/k93-Xauthority HOME=/root
test "$(id -u)" = 0
grep -q 'apple,k93ap' /proc/device-tree/compatible
grep -q '^overlay / overlay ' /proc/mounts
test -c /dev/fb0
test ! -S /tmp/.X11-unix/X0
test ! -e /tmp/.X0-lock
test ! -e "$XAUTHORITY"
test ! -e /run/k93-browser
test -x /usr/lib/firefox-esr/firefox-esr
test -x /usr/bin/xdotool
test -r /k93-browser-test.html
test -r /k93-browser-prefs.js
grep -q '^tmpfs /dev/shm tmpfs ' /proc/mounts
test "$(busybox stat -c %a /dev/shm)" = 1777
test "$(( $(cat /sys/class/net/lo/flags) & 1 ))" -eq 1
test "$(busybox hostname)" = "$(cat /etc/hostname)"
test -r /proc/sys/user/max_user_namespaces
test "$(cat /proc/sys/user/max_user_namespaces)" -gt 0
# Refuse to share a diagnostic UID with any pre-existing task.
if busybox pgrep -u 65534 >/dev/null; then exit 1; fi
test "$(id -u nobody)" = 65534
busybox su -s /bin/sh -c 'busybox unshare -Ur /bin/true' nobody
echo K93_BROWSER_BEGIN
echo K93_BROWSER_MEMORY_BEFORE
busybox free -m
umask 077
cookie=$(busybox timeout -k 1 5 /usr/bin/mcookie)
test "${#cookie}" = 32
case "$cookie" in *[!0123456789abcdefABCDEF]*) exit 1;; esac
touch "$XAUTHORITY"
/usr/bin/xauth -f "$XAUTHORITY" add :0 . "$cookie"
unset cookie
mkdir -m 700 /run/k93-browser /run/k93-browser/profile /run/k93-browser/runtime
cp "$XAUTHORITY" /run/k93-browser/Xauthority
cp /k93-browser-prefs.js /run/k93-browser/profile/user.js
chown -R 65534:65534 /run/k93-browser
/usr/bin/mkfontdir /usr/share/fonts/misc
# apk scripts/triggers were deliberately skipped. Generate only these known
# GUI caches into the writable RAM overlay, never run package scripts.
busybox timeout -k 1 8 /usr/bin/glib-compile-schemas --strict /usr/share/glib-2.0/schemas
busybox timeout -k 1 8 /usr/bin/gdk-pixbuf-query-loaders >/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache
server_pid= wm_pid= browser_pid=
failure_snapshot_done=0
startup_snapshot() {
    failure_snapshot_done=1
    echo K93_BROWSER_STARTUP_SNAPSHOT
    cat /proc/uptime
    busybox hostname
    busybox free -m
    # A failed page-ready test is NOT a successful desktop proof. Use separate
    # diagnostic markers so the strict success decoder cannot accept this.
    if busybox timeout -k 1 3 /k93-fb-read >/run/k93-diagnostic.ppm 2>/run/k93-diagnostic-fb.log; then
        echo K93_DIAGNOSTIC_FRAME_UPTIME
        cat /proc/uptime
        cat /run/k93-diagnostic-fb.log
        echo K93_DIAGNOSTIC_FB_PPM_BEGIN
        busybox gzip -c -1 /run/k93-diagnostic.ppm | busybox base64
        echo K93_DIAGNOSTIC_FB_PPM_END
    fi
    # Observe windows BEFORE slow process tracing or the browser lifetime cap.
    echo K93_BROWSER_WINDOW_SNAPSHOT_UPTIME
    cat /proc/uptime
    for display_pid in $(busybox pidof Xorg); do
        echo "K93_XSERVER_PROCESS $display_pid"
        busybox grep -E '^(Name|State|PPid|Uid|VmRSS|Threads):' "/proc/$display_pid/status" || :
        busybox cat "/proc/$display_pid/wchan" "/proc/$display_pid/syscall" || :
        busybox head -12 "/proc/$display_pid/stack" || :
        busybox ls -l "/proc/$display_pid/fd" || :
        for fd_meta in "/proc/$display_pid"/fdinfo/[0-9]*; do
            echo "K93_XSERVER_FDINFO ${fd_meta##*/}"
            busybox head -12 "$fd_meta" || :
        done
        if [ -x /k93-xorg-pc ]; then
            echo K93_XSERVER_PC_BEGIN
            busybox timeout -k 1 3 /k93-xorg-pc "$display_pid" || :
            echo K93_XSERVER_PC_END
        fi
        if [ -x /k93-strace ]; then
            echo K93_XSERVER_SYSCALL_METADATA_BEGIN
            LD_LIBRARY_PATH=/k93-strace-libs busybox timeout -k 1 1 /k93-strace -qq -e trace=poll,ppoll,epoll_wait,epoll_pwait,select,pselect6,futex -p "$display_pid" </dev/null 2>&1 || :
            echo K93_XSERVER_SYSCALL_METADATA_END
        fi
    done
    for visibility in visible all; do
        if [ "$visibility" = visible ]; then visible_arg=--onlyvisible; else visible_arg=; fi
        windows=$(busybox timeout -k 1 2 /usr/bin/xdotool search $visible_arg --name '.*' | busybox head -8) || windows=
        for id in $windows; do
            echo "K93_BROWSER_WINDOW $visibility $id"
            busybox timeout -k 1 1 /usr/bin/xdotool getwindowname "$id" || :
        done
    done
    if kill -0 "$browser_pid" 2>/dev/null; then
        echo K93_BROWSER_WRAPPER_ALIVE
    else
        if wait "$browser_pid"; then code=0; else code=$?; fi
        echo "K93_BROWSER_WRAPPER_EXIT=$code"
    fi
    traced=0
    for status in /proc/[0-9]*/status; do
        if busybox grep -q '^Uid:[[:space:]]*65534[[:space:]]' "$status"; then
            echo "K93_BROWSER_PROCESS $status"
            busybox grep -E '^(Name|State|PPid|Uid|VmRSS|VmSize|Threads):' "$status" || :
            busybox cat "${status%status}wchan" || :
            echo
            rollup="${status%status}smaps_rollup"
            if [ -r "$rollup" ]; then busybox grep -E '^(Rss|Pss|Pss_Anon|Pss_File|Swap):' "$rollup" || :; fi
            directory=${status%/status}
            busybox ls -l "$directory/fd" || :
            task_count=0
            for task in "$directory"/task/[0-9]*; do
                task_count=$((task_count+1))
                if [ "$task_count" -gt 8 ]; then echo K93_BROWSER_MORE_THREADS_OMITTED; break; fi
                echo "K93_BROWSER_THREAD $task"
                busybox grep -E '^(Name|State):' "$task/status" || :
                busybox cat "$task/wchan" || :
                echo
                busybox cat "$task/syscall" || :
                busybox head -12 "$task/stack" || :
            done
            if [ -x /k93-strace ] && [ "$traced" = 0 ]; then
                traced=1
                echo K93_BROWSER_SYSCALL_METADATA_BEGIN
                # No read/write/send/receive buffers, credentials or page data.
                LD_LIBRARY_PATH=/k93-strace-libs busybox timeout -k 1 3 /k93-strace -qq -f -e trace=poll,ppoll,connect,getsockopt -p "${directory##*/}" </dev/null 2>&1 || :
                echo K93_BROWSER_SYSCALL_METADATA_END
            fi
        fi
    done
    busybox head -40 /proc/net/unix || :
    for network in tcp tcp6 udp udp6 netlink; do
        echo "K93_BROWSER_SOCKET_TABLE $network"
        busybox head -40 "/proc/net/$network" || :
    done
    busybox ls -ld /dev/shm /run/k93-browser/profile || :
    busybox tail -30 /run/k93-browser.log || :
    busybox dmesg | busybox grep -Ei 'out of memory|oom-kill|killed process|segfault|undefined instruction|unhandled fault|denied|random:' | busybox tail -25 || :
    echo K93_BROWSER_STARTUP_SNAPSHOT_END
}
cleanup() {
    trap - EXIT INT TERM
    # UID was proven unused before we launched this diagnostic.
    busybox pkill -TERM -u 65534 2>/dev/null || :
    for pid in "$browser_pid" "$wm_pid" "$server_pid"; do
        if [ -n "$pid" ]; then kill "$pid" 2>/dev/null || :; fi
    done
    sleep 1
    busybox pkill -KILL -u 65534 2>/dev/null || :
    for pid in "$browser_pid" "$wm_pid" "$server_pid"; do
        if [ -n "$pid" ]; then wait "$pid" 2>/dev/null || :; fi
    done
    echo K93_DESKTOP_CLEANUP
}
finish() {
    finish_result=$?
    trap - EXIT INT TERM
    if [ "$finish_result" -ne 0 ] && [ "$failure_snapshot_done" = 0 ]; then
        startup_snapshot || :
    fi
    cleanup
    exit "$finish_result"
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
echo K93_DESKTOP_BEGIN
busybox timeout -k 2 125 /usr/libexec/Xorg :0 -dumbSched -config /k93-xorg.conf -auth "$XAUTHORITY" -nolisten tcp -noreset -logfile /run/k93-Xorg.log vt1 >/run/k93-Xorg-console.log 2>&1 &
server_pid=$!
count=0
while [ ! -S /tmp/.X11-unix/X0 ]; do
    kill -0 "$server_pid"
    count=$((count+1)); test "$count" -le 10; sleep 1
done
busybox timeout -k 2 120 /usr/bin/twm -f /k93-twm.conf >/run/k93-twm.log 2>&1 &
wm_pid=$!
echo K93_XSERVER_PREFLIGHT
busybox timeout -k 1 3 /usr/bin/xdotool getdisplaygeometry
# Verified in this exact ARM glxtest binary: skip GL hardware discovery on
# software-only fbdev. This is not a sandbox or process-isolation override.
busybox timeout -k 2 115 busybox su -s /bin/sh -c 'export HOME=/run/k93-browser XAUTHORITY=/run/k93-browser/Xauthority XDG_RUNTIME_DIR=/run/k93-browser/runtime XDG_CACHE_HOME=/tmp/k93-browser-cache GDK_BACKEND=x11 MOZ_ENABLE_WAYLAND=0 MOZ_AVOID_OPENGL_ALTOGETHER=1 NO_AT_BRIDGE=1 GSETTINGS_BACKEND=memory; exec /usr/lib/firefox-esr/firefox-esr --no-remote --profile /run/k93-browser/profile --width 1000 --height 700 file:///k93-browser-test.html' nobody >/run/k93-browser.log 2>&1 &
browser_pid=$!
if window=$(busybox timeout -k 1 100 /usr/bin/xdotool search --sync --onlyvisible --name '^K93_JS_READY([[:space:]].*)?$'); then
    :
else
    result=$?
    startup_snapshot
    exit "$result"
fi
test "$(printf '%s\n' "$window" | busybox wc -l)" -eq 1
window_pid=$(/usr/bin/xdotool getwindowpid "$window")
case "$window_pid" in ''|*[!0-9]*) exit 1;; esac
busybox grep -q '^Uid:[[:space:]]*65534[[:space:]]' "/proc/$window_pid/status"
echo K93_BROWSER_JS_READY
/usr/bin/xdotool windowfocus --sync "$window"
/usr/bin/xdotool type --clearmodifiers --delay 30 'k93-input'
/usr/bin/xdotool key Tab Return
passed=$(busybox timeout -k 1 10 /usr/bin/xdotool search --sync --onlyvisible --name '^K93_INPUT_PASS([[:space:]].*)?$')
test "$passed" = "$window"
echo K93_BROWSER_XTEST_INPUT_PASS
kill -0 "$server_pid"; kill -0 "$wm_pid"; kill -0 "$browser_pid"
echo K93_DESKTOP_PROCESSES_ALIVE
echo K93_BROWSER_MEMORY_AFTER
busybox free -m
for status in /proc/[0-9]*/status; do
    if busybox grep -q '^Uid:[[:space:]]*65534[[:space:]]' "$status"; then
        echo "K93_BROWSER_PROCESS $status"
        busybox grep -E '^(Name|Uid|VmRSS|VmSize|Threads):' "$status" || :
        rollup="${status%status}smaps_rollup"
        if [ -r "$rollup" ]; then busybox grep -E '^(Rss|Pss|Pss_Anon|Pss_File|Swap):' "$rollup" || :; fi
    fi
done
/k93-fb-read >/run/k93-screen.ppm 2>/run/k93-fb-info.log
echo K93_FB_PPM_BEGIN
busybox gzip -c -1 /run/k93-screen.ppm | busybox base64
echo K93_FB_PPM_END
cat /run/k93-fb-info.log
echo K93_BROWSER_END
echo K93_DESKTOP_END
