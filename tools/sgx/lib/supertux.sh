#!/bin/sh
# supertux.sh [ARGS] -- SuperTux on the GPU, with the touch gamepad.  Lives in
# the pack directory (rpack.py output + libsgxsdl.so).  While it runs, the
# console is unbound from the framebuffer and fbkeyboard is stopped (both
# draw on the screen); they come back when the game exits.
D=$(cd "$(dirname "$0")" && pwd)
mount -t debugfs none /sys/kernel/debug 2>/dev/null
echo 0 > /sys/class/vtconsole/vtcon1/bind
FBK=$(pidof fbkeyboard)
[ -n "$FBK" ] && kill -STOP $FBK
LD_PRELOAD="$D/libsgxsdl.so" SGXSDL_PACK="$D" SDL_VIDEODRIVER=offscreen \
	supertux2 --renderer sdl --geometry 1024x768 "$@"
[ -n "$FBK" ] && kill -CONT $FBK
echo 1 > /sys/class/vtconsole/vtcon1/bind
