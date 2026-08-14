FROM ghcr.io/embeddedcontainers/zephyr:arm-1.0.0SDK

USER root

RUN apt-get update && apt-get install -y \
    build-essential \
    python3-dev \
    vim \
    gdb \
    usbutils \
    udev \
    openssh-client \
    git \
    make \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /workspaces/LearningZephyr
WORKDIR /workspaces/LearningZephyr