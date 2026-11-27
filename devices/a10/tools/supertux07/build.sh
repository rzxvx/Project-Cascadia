#!/bin/sh
# SuperTux 0.7 for the iPad's Alpine (aarch64, musl; Alpine's own package is
# still 0.6.3): the release source, pinned by its sha256, built in docker
# (alpine:3.24, arm64) into /usr/local/supertux-0.7 -- beside the system's
# 0.6.3. The libraries it needs from Alpine, but not the GL ones (libGL /
# libEGL come from the driver: g9p-run), are put in its lib/ (RUNPATH
# $ORIGIN/../lib), so the iPad needs no packages for it.
#   build.sh            -> build/supertux07/supertux-0.7.tar.gz
#   build.sh install    and copy it to the iPad (HOST, default root@172.16.43.1)
# Run it there: supertux07 [ARGS] (g9p-run /usr/local/supertux-0.7/bin/supertux2)
set -e
A10="$(cd "$(dirname "$0")/../.." && pwd)"
HOST=${HOST:-root@172.16.43.1}
VER=0.7.0.1
SHA=c3a3813bc57b8267960e4fcc33afe5f5cb468abe2a288fe58da4f288d0d7442c
TAR=SuperTux-v$VER-Source.tar.gz
B="$A10/build/supertux07"
mkdir -p "$B"
if [ ! -f "$B/$TAR" ]; then
    curl -fL -o "$B/$TAR.part" "https://github.com/SuperTux/supertux/releases/download/v$VER/$TAR"
    mv "$B/$TAR.part" "$B/$TAR"
fi
echo "$SHA  $B/$TAR" | shasum -a 256 -c - >/dev/null || { echo "error: $TAR checksum" >&2; exit 1; }

# (the build tree in a docker volume: the bind mount is slow for it)
docker run --rm --platform linux/arm64 -v "$B:/out" -v "$A10/tools/supertux07:/src:ro" -v cascadia-a10-supertux07:/w \
  alpine:3.24 sh -ec "
  apk add --no-cache build-base cmake samurai pkgconf sdl2-dev sdl2_image-dev openal-soft-dev libogg-dev \
    libvorbis-dev physfs-dev fmt-dev glm-dev freetype-dev libraqm-dev glew-dev curl-dev libpng-dev zlib-dev \
    mesa-dev patchelf linux-headers >/dev/null
  cd /w
  [ -d SuperTux-v$VER-Source ] || tar xzf /out/$TAR
  # musl has no execinfo.h (the crash handler's stack trace): none, there
  E=SuperTux-v$VER-Source/src/supertux/error_handler.cpp
  grep -q g9p-musl \$E || sed -i 's|^#include <execinfo.h>|#if defined(__GLIBC__) /* g9p-musl */\\n#include <execinfo.h>\\n#else\\nstatic inline int backtrace(void **, int) { return 0; }\\nstatic inline char **backtrace_symbols(void *const *, int) { return nullptr; }\\n#endif|' \$E
  cmake -S SuperTux-v$VER-Source -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local/supertux-0.7 -DINSTALL_SUBDIR_BIN=bin -DENABLE_DISCORD=OFF \
    -DCMAKE_INSTALL_RPATH='\$ORIGIN/../lib' >/w/cmake.log
  ninja -C build >/w/ninja.log
  rm -rf /w/root && DESTDIR=/w/root ninja -C build install >/dev/null
  P=/w/root/usr/local/supertux-0.7
  mkdir -p \$P/lib
  # (the image's Mesa came in with libGL: its own libraries are not needed)
  # the shared libraries it needs from the image, minus musl and GL
  ldd \$P/bin/supertux2 | awk '/=> \\// { print \$3 }' | while read -r l; do
    case \"\${l##*/}\" in
      ld-musl*|libc.musl*|libGL.*|libGLX*|libEGL*|libGLdispatch*|libgbm*|libglapi*|libdrm*|libX11*|libxcb*|libXau*|libXdmcp*|libwayland*) ;;
      libgallium*|libLLVM*|libSPIRV*|libxshmfence*|libelf*|libexpat*|libXext*|libXxf86vm*|libffi*|libxml2*|libzstd*|liblzma*) ;;
      *) [ -e \"\$P/lib/\${l##*/}\" ] || cp -L \"\$l\" \$P/lib/ ;;
    esac
  done
  patchelf --set-rpath '\$ORIGIN/../lib' \$P/bin/supertux2
  strip \$P/bin/supertux2
  # vkeys: a uinput keyboard playing key scripts (driving it hands-free)
  gcc -O2 -Wall -o \$P/bin/vkeys /src/vkeys.c
  tar -C /w/root/usr/local -czf /out/supertux-0.7.tar.gz supertux-0.7
"
echo "==> $B/supertux-0.7.tar.gz"
if [ "${1:-}" = install ]; then
    scp -q "$B/supertux-0.7.tar.gz" "$HOST:/tmp/supertux-0.7.tar.gz"
    ssh "$HOST" 'set -e; rm -rf /usr/local/supertux-0.7 && tar -C /usr/local -xzf /tmp/supertux-0.7.tar.gz &&
      rm /tmp/supertux-0.7.tar.gz && du -sh /usr/local/supertux-0.7 &&
      ln -sf /usr/local/supertux-0.7/bin/vkeys /usr/local/bin/vkeys &&
      printf "#!/bin/sh\n# SuperTux 0.7 on the GPU (g9p): tools/supertux07/build.sh install\nexec /usr/local/g9p/bin/g9p-run /usr/local/supertux-0.7/bin/supertux2 \"\$@\"\n" \
        > /usr/local/bin/supertux07 && chmod 755 /usr/local/bin/supertux07'
    echo "==> $HOST:/usr/local/supertux-0.7"
fi
