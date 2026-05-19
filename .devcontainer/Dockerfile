FROM ghcr.io/zephyrproject-rtos/ci:v0.29.2

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    git \
    cmake \
    ninja-build \
    gperf \
    ccache \
    dfu-util \
    device-tree-compiler \
    wget \
    python3-pip \
    python3-setuptools \
    python3-wheel \
    python3-venv \
    xz-utils \
    file \
    make \
    gcc \
    gcc-multilib \
    g++-multilib \
    libsdl2-dev \
    libmagic1 \
    && rm -rf /var/lib/apt/lists/*

RUN pip3 install --upgrade pip west

WORKDIR /opt

RUN west init zephyrproject \
    && cd zephyrproject \
    && west update \
    && west zephyr-export

RUN wget -q https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v0.17.0/zephyr-sdk-0.17.0_linux-x86_64.tar.xz \
    && tar xf zephyr-sdk-0.17.0_linux-x86_64.tar.xz \
    && mv zephyr-sdk-0.17.0 /opt/zephyr-sdk \
    && rm zephyr-sdk-0.17.0_linux-x86_64.tar.xz

RUN /opt/zephyr-sdk/setup.sh -t all -h

ENV ZEPHYR_BASE=/opt/zephyrproject/zephyr
ENV ZEPHYR_TOOLCHAIN_VARIANT=zephyr
ENV ZEPHYR_SDK_INSTALL_DIR=/opt/zephyr-sdk