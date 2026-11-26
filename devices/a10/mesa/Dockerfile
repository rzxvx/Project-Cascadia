# The image Mesa is built in for the iPad 7: Alpine's own aarch64 userland,
# so the build is native musl with the device root's libraries (and native
# speed on an Apple Silicon Mac).
#   docker build --platform linux/arm64 -t cascadia-a10-mesa devices/a10/mesa
FROM alpine:3.24

RUN apk add --no-cache \
        build-base meson samurai pkgconf git \
        python3 py3-mako py3-yaml py3-packaging \
        bison flex \
        libdrm-dev zlib-dev linux-headers expat-dev zstd-dev \
        wayland-dev wayland-protocols
WORKDIR /a10
