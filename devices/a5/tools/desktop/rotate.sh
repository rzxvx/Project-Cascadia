#!/bin/sh
# rotate [normal|left|inverted|right] -- the X screen turned, and the touch
# screen with it (docs/research/p105-mesa.md, M41).  On the iPad as
# /usr/local/bin/rotate; with no argument it says how the screen is turned.
#
# The turn is the KMS plane's (apple-sgx turns its copy onto the screen and
# the patched modesetting, tools/desktop/xorg-server, hands RandR's
# rotation to it): no shadow buffer, every frame shown.  The touch screen
# reports the panel's own coordinates, so libinput's Coordinate
# Transformation Matrix takes them to the turned screen: for RandR's
# anticlockwise turns, x' = 1 - v, y' = u for left (90), and so on round.
export DISPLAY="${DISPLAY:-:0}"
OUT=$(xrandr | awk '/ connected/ { print $1; exit }')
TOUCH="S5L8940X Capacitive TouchScreen"
case "${1:-}" in
    "")        # (the turn is the field before xrandr's list of them, in brackets)
               xrandr | awk -v o="$OUT" '$1 == o { for (i = 1; i <= NF && $i !~ /^\(/; i++) if ($i ~ /^(left|right|inverted)$/) { print $i; exit } print "normal"; exit }'; exit ;;
    normal)    M="1 0 0 0 1 0 0 0 1" ;;
    left)      M="0 -1 1 1 0 0 0 0 1" ;;
    inverted)  M="-1 0 1 0 -1 1 0 0 1" ;;
    right)     M="0 1 0 -1 0 1 0 0 1" ;;
    *)         sed -n '2,4p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
xrandr --output "$OUT" --rotate "$1" || exit 1
# shellcheck disable=SC2086
xinput set-prop "$TOUCH" "Coordinate Transformation Matrix" $M 2>/dev/null ||
    echo "rotate: no \"$TOUCH\" to turn" >&2
