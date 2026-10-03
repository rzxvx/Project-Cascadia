# The image Mesa is built in for the iPad: Alpine's own armv7 userland, so
# the build is native musl with the same libraries the device's root has.
# On an x86_64 or Apple Silicon host it runs under ARM emulation (Docker
# Desktop has it; on a Linux host, qemu-user-static's binfmt handlers).
#   docker build --platform linux/arm/v7 -t cascadia-mesa mesa
FROM alpine:3.24

RUN apk add --no-cache \
        build-base meson samurai pkgconf git \
        python3 py3-mako py3-yaml py3-packaging \
        bison flex \
        libdrm-dev zlib-dev linux-headers

WORKDIR /cascadia
