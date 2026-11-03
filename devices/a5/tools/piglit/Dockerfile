# The image piglit is built in for the iPad (tools/piglit/build.sh):
# Alpine's armv7 userland, as mesa/Dockerfile's, with piglit's build tools.
#   docker build --platform linux/arm/v7 -t cascadia-piglit tools/piglit
FROM alpine:3.24

RUN apk add --no-cache \
        build-base cmake samurai pkgconf python3 py3-mako py3-numpy py3-lxml \
        waffle-dev mesa-dev libx11-dev libxkbcommon-dev libxrender-dev \
        libpng-dev libdrm-dev wayland-dev wayland-protocols zlib-dev

WORKDIR /cascadia
