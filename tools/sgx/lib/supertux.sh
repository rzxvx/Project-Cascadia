#!/bin/sh
# supertux.sh [ARGS] -- SuperTux on the GPU, with the touch gamepad.  Lives in
# the pack directory (tools/sgx/mkpack.py; installed as supertux-gpu).  While
# it runs, the console is unbound from the framebuffer and fbkeyboard is
# stopped -- both draw on the screen.  Afterwards the console comes back and
# fbkeyboard is started again the way stage 2 started it (its arguments are
# in /run/fbkeyboard.args), as the XFCE launcher does.
D=$(dirname "$(readlink -f "$0")")
mount -t debugfs none /sys/kernel/debug 2>/dev/null
if [ ! -e /sys/kernel/debug/apple-sgx/mem ]; then
	echo "supertux-gpu: no apple-sgx GPU driver (kernel too old?)" >&2
	exit 1
fi
echo 0 > /sys/class/vtconsole/vtcon1/bind
# fbkeyboard only looks at SIGTERM when its next touch arrives -- which
# would be a touch meant for the game, typed into the console underneath
pkill -x fbkeyboard 2>/dev/null
sleep 0.3
pkill -9 -x fbkeyboard 2>/dev/null
# --geometry is also SuperTux's fullscreen size, and it keeps it in its
# config: the desktop's SuperTux then asked for a 1024 x 768 screen the
# portrait one is not, and drew its menu off to the right.  Put back what
# was there (0 x 0: the desktop's size).
C="${XDG_DATA_HOME:-$HOME/.local/share}/supertux2/config"
FW=$(sed -n 's/.*(fullscreen_width \([0-9]*\)).*/\1/p' "$C" 2>/dev/null)
FH=$(sed -n 's/.*(fullscreen_height \([0-9]*\)).*/\1/p' "$C" 2>/dev/null)
LD_PRELOAD="$D/libsgxsdl.so" SGXSDL_PACK="$D" SDL_VIDEODRIVER=offscreen \
	supertux2 --renderer sdl --geometry 1024x768 "$@"
status=$?
if [ -f "$C" ]; then
	sed -i "s/(fullscreen_width [0-9]*)/(fullscreen_width ${FW:-0})/;
		s/(fullscreen_height [0-9]*)/(fullscreen_height ${FH:-0})/" "$C"
fi
echo 1 > /sys/class/vtconsole/vtcon1/bind
if [ -s /run/fbkeyboard.args ] && ! pidof fbkeyboard >/dev/null; then
	setsid sh -c 'exec fbkeyboard $(cat /run/fbkeyboard.args) >/tmp/fbkeyboard.log 2>&1 </dev/null' &
fi
exit $status
