#!/bin/sh
# autorotate -- the screen turned the way the iPad is held.  Started with
# each XFCE session (/etc/xdg/autostart/cascadia-autorotate.desktop), one
# for each X server, and gone with it; on the iPad as /usr/local/bin/
# autorotate (tools/desktop/install.sh).
#
# The accelerometer is an IIO device (an ST LIS3DSH, dts/p105ap.dts), read
# through sysfs three times a second; rotate (rotate.sh) turns the screen
# and the touch screen.  The rotation lock is the file /run/rotation-lock:
# while it is there nothing turns.  The panel's button (rotation-lock
# toggle) and a long press of Home (p105-keys) make and remove it; this
# shows which on the button, and puts the button on the panel the first
# time.
#
# Which way: the push against gravity in the screen's axes (the device's
# mount matrix applied: x right, y up -- away from Home -- and z out of the
# glass).  Lying flat (less than half of it in the glass's plane) says
# nothing, the turn stays; otherwise the larger of x and y, when the other
# is under 2/3 of it (34 degrees off the axis -- a diagonal does not
# flicker), and the same twice running: y > 0 normal, y < 0 inverted,
# x < 0 left (RandR's: the picture's top at the panel's left), x > 0 right.
export DISPLAY="${DISPLAY:-:0}"
export PATH="/usr/local/bin:$PATH"
LOCK=/run/rotation-lock
LAUNCHERS="$HOME/.config/xfce4/panel"

dev=
for d in /sys/bus/iio/devices/iio:device*; do
    [ -r "$d/in_accel_x_raw" ] && { dev=$d; break; }
done
[ -n "$dev" ] || { echo "autorotate: no accelerometer (no IIO device with in_accel_x_raw)" >&2; exit 1; }
# the X server, to end with it (its lock file holds its pid)
xpid=
read -r xpid 2>/dev/null < "/tmp/.X$(echo "${DISPLAY#*:}" | cut -d. -f1)-lock"
[ -n "$xpid" ] || { echo "autorotate: no X server on $DISPLAY" >&2; exit 1; }
pidf="/run/autorotate-$xpid.pid"
if read -r p 2>/dev/null < "$pidf" && [ "$p" != $$ ] && kill -0 "$p" 2>/dev/null; then
    echo "autorotate: running already ($p)" >&2
    exit 0
fi
echo $$ > "$pidf"
trap 'rm -f "$pidf"' EXIT
trap 'exit 0' INT TERM HUP

# A raw read wakes the chip and waits two of its samples: at the driver's
# first rate, 3 Hz, two thirds of a second an axis.
echo 100 > "$dev/sampling_frequency" 2>/dev/null ||
    echo "autorotate: cannot set $dev/sampling_frequency; reads will be slow" >&2

m="1 0 0 0 1 0 0 0 1"
for f in "$dev/in_accel_mount_matrix" "$dev/mount_matrix"; do
    if [ -r "$f" ]; then
        IFS=', ;' read -r a b c d e f2 g h i < "$f"
        m="$a $b $c $d $e $f2 $g $h $i"
        break
    fi
done
for v in $m; do
    case "$v" in -1|0|1) ;; *) echo "autorotate: mount matrix [$m] not of -1, 0, 1; the identity then" >&2
                               m="1 0 0 0 1 0 0 0 1"; break ;; esac
done
# shellcheck disable=SC2086
set -- $m

# the button's icon and name: the lock's state
icon() {
    if [ -e "$LOCK" ]; then i=cascadia-rotation-locked n="Rotation locked"
    else i=cascadia-rotation n="Rotation: automatic"; fi
    for f in "$LAUNCHERS"/launcher-*/rotation-lock.desktop; do
        [ -f "$f" ] && ! grep -qx "Icon=$i" "$f" &&
            sed -i "s/^Icon=.*/Icon=$i/; s/^Name=.*/Name=$n/" "$f"
    done
}

# The button, once: next to the keyboard's (or before the clock).  After
# xfce-setup.sh's first run has put the keyboard's there -- both edit the
# panel's list of plugins.
panel_button() {
    ls "$LAUNCHERS"/launcher-*/rotation-lock.desktop >/dev/null 2>&1 && return
    [ -f /usr/share/applications/rotation-lock.desktop ] || return
    n=0
    while [ -e /etc/xdg/autostart/cascadia-xfce-firstrun.desktop ] &&
          ! ls "$LAUNCHERS"/launcher-*/kbd-toggle.desktop >/dev/null 2>&1; do
        n=$((n + 1)); [ $n -gt 60 ] && break
        sleep 1
    done
    n=0
    until ids=$(xfconf-query -c xfce4-panel -p /panels/panel-1/plugin-ids 2>/dev/null | tail -n +3) && [ -n "$ids" ]; do
        n=$((n + 1)); [ $n -gt 30 ] && return
        sleep 1
    done
    max=0
    for k in $(xfconf-query -c xfce4-panel -l | sed -n 's|^/plugins/plugin-\([0-9]*\)$|\1|p'); do
        [ "$k" -gt "$max" ] && max=$k
    done
    new=$((max + 1))
    kbd=$(ls "$LAUNCHERS"/launcher-*/kbd-toggle.desktop 2>/dev/null | sed -n '1s|.*/launcher-\([0-9]*\)/.*|\1|p')
    mkdir -p "$LAUNCHERS/launcher-$new"
    cp /usr/share/applications/rotation-lock.desktop "$LAUNCHERS/launcher-$new/"
    icon
    xfconf-query -c xfce4-panel -p /plugins/plugin-$new -n -t string -s launcher
    xfconf-query -c xfce4-panel -p /plugins/plugin-$new/items -n -a -t string -s rotation-lock.desktop
    args="" placed=""
    for id in $ids; do
        if [ -z "$placed" ] && [ -z "$kbd" ] && [ "$(xfconf-query -c xfce4-panel -p /plugins/plugin-$id)" = clock ]; then
            args="$args -t int -s $new"; placed=1
        fi
        args="$args -t int -s $id"
        if [ -z "$placed" ] && [ "$id" = "$kbd" ]; then
            args="$args -t int -s $new"; placed=1
        fi
    done
    [ -n "$placed" ] || args="$args -t int -s $new"
    # shellcheck disable=SC2086
    xfconf-query -c xfce4-panel -p /panels/panel-1/plugin-ids $args
    xfce4-panel -r
}
panel_button &

locked="" seen="" last=""
while kill -0 "$xpid" 2>/dev/null; do
    if [ -e "$LOCK" ]; then l=1; else l=0; fi
    if [ "$l" != "$locked" ]; then
        locked=$l
        icon
        [ "$l" = 0 ] && seen=""         # unlocked: the way it is held now, at once
    fi
    read -r x < "$dev/in_accel_x_raw" && read -r y < "$dev/in_accel_y_raw" &&
        read -r z < "$dev/in_accel_z_raw" || { sleep 1; continue; }
    # the screen's axes, in 1/16 of the raw (a g is ~1000 of them at 2 g)
    X=$((($1 * x + $2 * y + $3 * z) / 16))
    Y=$((($4 * x + $5 * y + $6 * z) / 16))
    Z=$((($7 * x + $8 * y + $9 * z) / 16))
    ax=${X#-} ay=${Y#-} o=""
    if [ $((4 * (X * X + Y * Y))) -ge $((X * X + Y * Y + Z * Z)) ]; then
        if [ $((3 * ax)) -lt $((2 * ay)) ]; then
            if [ "$Y" -gt 0 ]; then o=normal; else o=inverted; fi
        elif [ $((3 * ay)) -lt $((2 * ax)) ]; then
            if [ "$X" -lt 0 ]; then o=left; else o=right; fi
        fi
    fi
    if [ -n "$o" ] && [ "$o" = "$last" ] && [ "$o" != "$seen" ]; then
        seen=$o
        [ "$locked" = 0 ] && [ "$(rotate)" != "$o" ] && rotate "$o"
    fi
    last=$o
    sleep 0.25
done
