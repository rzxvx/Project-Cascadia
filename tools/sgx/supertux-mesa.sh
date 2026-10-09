#!/bin/sh
# supertux-mesa.sh [ARGS] -- SuperTux through the Mesa driver: SDL's
# renderer on GLES 2, on KMS (docs/research/p105-mesa.md, M25).  The console
# and fbkeyboard are taken off the screen while it runs, as supertux.sh does.
#
#   SGX_DEBUG=fps supertux-mesa.sh       the frame rate, every 2 s
#   TIME=30 supertux-mesa.sh             at most 30 s
#   WRAP=/root/sgx-dev supertux-mesa.sh  another build's Mesa
#
# SuperTux links the system's libGL (GLEW): sgx-gl preloads ours
# (SGX_PRELOAD=1), or the system's Mesa takes the calls.
echo 0 > /sys/class/vtconsole/vtcon1/bind
pkill -x fbkeyboard 2>/dev/null; sleep 0.3; pkill -9 -x fbkeyboard 2>/dev/null
SDL_VIDEODRIVER=kmsdrm SDL_RENDER_DRIVER=opengles2 SGX_PRELOAD=1 \
	timeout "${TIME:-86400}" ${WRAP:-sgx-gl} supertux2 --renderer sdl --fullscreen "$@"
s=$?
echo 1 > /sys/class/vtconsole/vtcon1/bind
exit $s
