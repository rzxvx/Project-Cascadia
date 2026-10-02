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
LD_PRELOAD="$D/libsgxsdl.so" SGXSDL_PACK="$D" SDL_VIDEODRIVER=offscreen \
	supertux2 --renderer sdl --geometry 1024x768 "$@"
status=$?
echo 1 > /sys/class/vtconsole/vtcon1/bind
if [ -s /run/fbkeyboard.args ] && ! pidof fbkeyboard >/dev/null; then
	setsid sh -c 'exec fbkeyboard $(cat /run/fbkeyboard.args) >/tmp/fbkeyboard.log 2>&1 </dev/null' &
fi
exit $status
