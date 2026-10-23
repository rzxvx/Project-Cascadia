#!/bin/sh
# tools/piglit/install.sh -- piglit (tools/piglit/build.sh's build) onto the
# iPad: /root/piglit, run.sh and pigsum.py beside it.  Left out: the
# generated tests of GLSL 4.x and of 64-bit types, 1.8 GB of the 2 GB, which
# a GL 2.1 part never runs (run.sh excludes them too).  COPYFILE_DISABLE: a
# Mac's tar adds a ._NAME of extended attributes for each file, and piglit's
# framework imports every *.py it finds -- "._json.py" broke it.
#
# piglit needs waffle and mako on the iPad: tools/device-apk.sh waffle
# py3-mako py3-markupsafe.  DEVICE=root@10.55.0.2 (the default).
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEVICE="${DEVICE:-root@10.55.0.2}"
I="$ROOT/build/piglit-install/usr"
[ -d "$I/lib/piglit" ] || { echo "no piglit in build/piglit-install (tools/piglit/build.sh)" >&2; exit 1; }
COPYFILE_DISABLE=1 tar cf - -C "$I" --exclude 'generated_tests/spec/glsl-4.*' \
    --exclude 'generated_tests/spec/arb_vertex_attrib_64bit' \
    --exclude 'generated_tests/spec/arb_gpu_shader_*64*' . |
    ssh "$DEVICE" 'mkdir -p /root/piglit /root/piglit-results && tar xf - -C /root/piglit &&
                   find /root/piglit -name "._*" -delete'
for f in run.sh pigsum.py; do
    ssh "$DEVICE" "cat > /root/piglit/$f && chmod 755 /root/piglit/$f" < "$ROOT/tools/piglit/$f"
done
ssh "$DEVICE" 'cp /root/piglit/pigsum.py /root/pigsum.py; du -sh /root/piglit'
