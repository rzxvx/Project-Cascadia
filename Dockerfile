# Нативный arm64-образ (без эмуляции на Apple Silicon)
FROM --platform=linux/arm64 ubuntu:22.04

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
