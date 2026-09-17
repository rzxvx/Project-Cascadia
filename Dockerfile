# Build image for the cross-compiler and the host-side tooling.
#
# No --platform pin: it used to say linux/arm64 for an Apple Silicon host, which
# forces qemu emulation on an x86_64 machine -- or fails outright.  Docker uses
# the host architecture by default, and gcc-arm-linux-gnueabihf is packaged for
# both amd64 and arm64 hosts, so the cross-compile works either way.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    gcc-arm-linux-gnueabihf \
    g++-arm-linux-gnueabihf \
    binutils-arm-linux-gnueabihf \
    bc \
    bison \
    flex \
    libssl-dev \
    libncurses-dev \
    device-tree-compiler \
    git \
    curl \
    wget \
    python3 \
    python3-pip \
    libelf-dev \
    rsync \
    cpio \
    unzip \
    xz-utils \
    && rm -rf /var/lib/apt/lists/*

# Инструменты для работы с Apple firmware (xpwntool, img4tool и т.п. собираются отдельно,
# т.к. некоторые требуют доп. библиотек — добавим по мере необходимости)

ENV ARCH=arm
ENV CROSS_COMPILE=arm-linux-gnueabihf-

WORKDIR /work

CMD ["/bin/bash"]
