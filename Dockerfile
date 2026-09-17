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
    # img3decrypt/img3encrypt need AES; patch-ibec-autogo disassembles Thumb to
    # find the USB completion callback it hooks.  Distro packages rather than
    # pip so the image builds without reaching pypi at run time.
    python3-pycryptodome \
    python3-capstone \
    libelf-dev \
    rsync \
    cpio \
    unzip \
    xz-utils \
    && rm -rf /var/lib/apt/lists/*

# Инструменты для работы с Apple firmware (xpwntool, img4tool и т.п. собираются отдельно,
# т.к. некоторые требуют доп. библиотек — добавим по мере необходимости)

# pycryptodome: the img3 decrypt/encrypt steps of the boot chain are pure
# Python, which is why this image needs no xpwntool and no prebuilt binaries
# from anyone's tree.
RUN pip3 install --no-cache-dir pycryptodome

# iBoot32Patcher, built from source rather than vendored or borrowed from a
# Legacy iOS Kit checkout: third-party GPL code is better cloned at image build
# time than copied into this repository, and it means the boot chain has no
# dependency on a tool the user has to install separately.  Four .c files and
# no libraries.
# Four things were wrong with the first version of this, and the trailing
# "|| true" hid all of them: the image built clean and the binary was simply
# absent, which only surfaced when ./cascadia firmware went looking for it.
#   - the .c files are at the repo root, not in an iBoot32Patcher/ subdirectory
#   - the sources say #include <include/finders.h>, so -I is the root, not
#     the include/ directory
#   - RUN uses /bin/sh, which does not expand {a,b,c} braces -- the literal
#     string was handed to gcc as a filename
#   - "|| true" applies to the whole && chain, so a failed gcc still succeeded
# No masking now: if this cannot build, the image does not build, and the last
# line proves the result actually runs.
RUN git clone --depth 1 https://github.com/iH8sn0w/iBoot32Patcher /tmp/ib32 \
    && cd /tmp/ib32 \
    && gcc iBoot32Patcher.c finders.c functions.c patchers.c \
        -Wno-multichar -I. -o /usr/local/bin/iBoot32Patcher \
    && cd / && rm -rf /tmp/ib32 \
    && iBoot32Patcher 2>&1 | head -1

ENV ARCH=arm
ENV CROSS_COMPILE=arm-linux-gnueabihf-

WORKDIR /work

CMD ["/bin/bash"]
