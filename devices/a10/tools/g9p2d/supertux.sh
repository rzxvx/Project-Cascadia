#!/bin/sh
# supertux.sh [ARGS] -- SuperTux on the GPU (g9p2d + g9psdl), with the touch
# gamepad; installed as /usr/local/bin/supertux-gpu. While it runs the
# console is unbound from the display and a desktop on it is frozen (both
# would draw over the game).
D=$(dirname "$(readlink -f "$0")")
if [ ! -e /dev/dri/renderD128 ]; then
	echo "supertux-gpu: no GPU (/dev/dri/renderD128; modprobe powervr)" >&2
	exit 1
fi
echo 0 > /sys/class/vtconsole/vtcon1/bind 2>/dev/null
# a Wayland desktop on the same display would draw over the game: frozen
# while it runs (its windows come back on their next redraw)
DESK=$(pidof labwc swaybg waybar 2>/dev/null)
[ -n "$DESK" ] && kill -STOP $DESK
# --geometry is also SuperTux's fullscreen size and it keeps it in its
# config: put back what was there afterwards
C="${XDG_DATA_HOME:-$HOME/.local/share}/supertux2/config"
FW=$(sed -n 's/.*(fullscreen_width \([0-9]*\)).*/\1/p' "$C" 2>/dev/null)
FH=$(sed -n 's/.*(fullscreen_height \([0-9]*\)).*/\1/p' "$C" 2>/dev/null)
LD_PRELOAD="$D/libg9psdl.so" G9P2D_SHADERS="$D/g9p2d.bin" SDL_VIDEODRIVER=offscreen \
	supertux2 --renderer sdl --geometry "${G9PSDL_GEOMETRY:-1080x810}" "$@"
status=$?
if [ -f "$C" ]; then
	sed -i "s/(fullscreen_width [0-9]*)/(fullscreen_width ${FW:-0})/;
		s/(fullscreen_height [0-9]*)/(fullscreen_height ${FH:-0})/" "$C"
fi
[ -n "$DESK" ] && kill -CONT $DESK
echo 1 > /sys/class/vtconsole/vtcon1/bind 2>/dev/null
exit $status
