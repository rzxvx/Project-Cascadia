#!/bin/sh
# rotate [normal|left|inverted|right] -- the screen turned, and the touch
# screen with it.  On the iPad as /usr/local/bin/rotate; with no argument it
# says how the screen is turned.  The desktop's (labwc) below first; then an
# X server's (the iPad 2's XFCE, docs/research/p105-mesa.md, M41).
#
# Under X the turn is the KMS plane's (apple-sgx turns its copy onto the screen and
# the patched modesetting, tools/desktop/xorg-server, hands RandR's
# rotation to it): no shadow buffer, every frame shown.  The touch screen
# reports the panel's own coordinates, so libinput's Coordinate
# Transformation Matrix takes them to the turned screen: for RandR's
# anticlockwise turns, x' = 1 - v, y' = u for left (90), and so on round.
#
# Under the Wayland desktop (labwc, no Xorg) the turn is the output's
# transform instead (wlr-randr): the compositor draws the picture turned, the
# plane stays as it is, and wlroots turns the touches of a touch screen
# mapped to the output itself.  RandR's left (the picture's top at the
# panel's left) is Wayland's 90, its right 270.
if pidof labwc >/dev/null && ! pidof Xorg >/dev/null; then
    export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/xdg-0}" WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
    OUT=$(wlr-randr | awk 'NR == 1 { print $1; exit }')
    case "${1:-}" in
        "")       wlr-randr | awk '$1 == "Transform:" { t = $2 } END {
                      print (t == "90" ? "left" : t == "180" ? "inverted" : t == "270" ? "right" : "normal") }'
                  exit ;;
        normal)   T=normal ;;
        left)     T=90 ;;
        inverted) T=180 ;;
        right)    T=270 ;;
        *)        sed -n '2,4p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
    esac
    wlr-randr --output "$OUT" --transform "$T" || exit 1
    # for what draws over the screen by itself (task-killer): labwc turns
    # the picture, the plane says nothing of it
    echo "$1" > /run/cascadia-rotation
    exit 0
fi
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
echo "$1" > /run/cascadia-rotation
# shellcheck disable=SC2086
xinput set-prop "$TOUCH" "Coordinate Transformation Matrix" $M 2>/dev/null ||
    echo "rotate: no \"$TOUCH\" to turn" >&2
