#!/bin/sh
# tools/piglit/shaders.sh NAME MESA [130] GROUP... -- on the iPad: piglit's
# shader tests (quick_shader: shader_runner's, the built-in functions') and
# quick_gl's, for the groups given, with the build in MESA (beside the
# system's: mesa/devbuild.sh), into /root/piglit-results/NAME
# (docs/research/p105-mesa.md, M44).  130: as OpenGL 3.0 and GLSL 1.30
# (MESA_GL_VERSION_OVERRIDE), for GLSL 1.30's tests while the driver is
# OpenGL 2.1 -- a test's tool, not the driver's version.
#
#   shaders.sh m44-base /root/sgx-mesa-base spec@glsl-1.10 spec@glsl-1.20
#   shaders.sh m44-130 /root/sgx-mesa-rel 130 spec@glsl-1.30
#   python3 /root/pigsum.py /root/piglit-results/NAME pass   (comm the lists)
set -eu
NAME=$1; M=$2; shift 2
export LD_LIBRARY_PATH=/root/piglit/lib/piglit/lib PIGLIT_PLATFORM=gbm SGX_MESA=$M
if [ "${1:-}" = 130 ]; then
	export MESA_GL_VERSION_OVERRIDE=3.0 MESA_GLSL_VERSION_OVERRIDE=130
	shift
fi
T=""
for t in "$@"; do T="$T -t $t"; done
# shellcheck disable=SC2086
exec /root/sgx-dev python3 /root/piglit/bin/piglit run -p gbm --timeout 300 \
	quick_shader quick_gl "/root/piglit-results/$NAME" $T -x streaming-texture-leak
