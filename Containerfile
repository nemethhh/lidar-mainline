# SPDX-License-Identifier: GPL-2.0-only
FROM docker.io/library/debian:trixie
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      gcc-aarch64-linux-gnu gcc make bc bison flex libssl-dev libgnutls28-dev uuid-dev \
      python3 python3-dev python3-setuptools python3-pyelftools python3-libfdt swig \
      device-tree-compiler git ca-certificates cpio kmod rsync xz-utils file \
      curl e2fsprogs libc6-dev-arm64-cross clang lld cmake ninja-build \
    && rm -rf /var/lib/apt/lists/*
ENV ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
WORKDIR /src
