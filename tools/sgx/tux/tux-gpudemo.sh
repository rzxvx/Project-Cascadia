#!/bin/sh
# tux-gpudemo [SECONDS [PERIOD]] -- the GPU demo (tuxorbit.c): Tux going
# round on the screen through OpenGL ES and KMS, until its EXIT button.
# Installed with the Mesa (./cascadia mesa) as /usr/local/bin/tux-gpudemo,
# the scene in ../share/tux-gpudemo.  While it runs, the console is unbound
# from the framebuffer and fbkeyboard is stopped -- both draw on the screen
# -- and they come back however it ends, Ctrl-C and kill too.  Needs no X
# server on the screen.
D=$(dirname "$(readlink -f "$0")")
back() {
	echo 1 > /sys/class/vtconsole/vtcon1/bind
	if [ -s /run/fbkeyboard.args ] && ! pidof fbkeyboard >/dev/null; then
		setsid sh -c 'exec fbkeyboard $(cat /run/fbkeyboard.args) >/tmp/fbkeyboard.log 2>&1 </dev/null' &
	fi
}
echo 0 > /sys/class/vtconsole/vtcon1/bind
pkill -x fbkeyboard 2>/dev/null
sleep 0.3
pkill -9 -x fbkeyboard 2>/dev/null
# (in the background, waited for: a signal to this script stops the demo at
# once -- a foreground one would run on, the trap only after it)
"$D/tuxorbit" "$@" &
demo=$!
trap 'kill $demo 2>/dev/null' INT TERM HUP
wait $demo
status=$?
wait $demo 2>/dev/null
back
exit $status
