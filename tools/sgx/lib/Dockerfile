# The build environment for tools/sgx/lib: the device's own userland
# (Alpine, armv7, musl), emulated on the build machine.
#   docker build --platform linux/arm/v7 -t cascadia-armdev tools/sgx/lib
FROM alpine:3.24
RUN apk add --no-cache gcc musl-dev make sdl2-compat-dev linux-headers
