# Reproduces the Linux GCC lane of .github/workflows/build.yml so CI
# failures can be debugged locally instead of round-tripping through
# GitHub. Pinned to ubuntu:24.04 (= ubuntu-latest at time of writing)
# and CMake 3.31 from Kitware's release tarball — the apt cmake on
# 24.04 is 3.28, older than the project's cmake_minimum_required and
# old enough that unity-build batching diverges from CI.
#
# Build the image:
#   docker build -t eacp-ci-linux .
#
# Reproduce the GH Actions Linux GCC lane in one shot:
#   docker run --rm -v "$PWD":/workspace eacp-ci-linux ci-build
#
# Or drop into a shell to iterate:
#   docker run --rm -it -v "$PWD":/workspace eacp-ci-linux
#   # then inside:
#   ci-build              # runs configure + build + ctest like CI
#   # or step through manually with cmake/ninja

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG CMAKE_VERSION=3.31.6

# Mirrors the apt install line in build.yml plus build-essential
# (gcc/g++/make), gdb for debugging, ca-certificates/curl/git for
# CMake FetchContent, and Mesa's lavapipe (mesa-vulkan-drivers) with the
# Vulkan loader, tools and validation layers so the Vulkan backend's tests
# run headless on a software device, as the Windows lane runs on WARP.
# CMake itself comes from Kitware to match the version GH Actions ships.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        clang \
        curl \
        g++ \
        gcc \
        gdb \
        git \
        libcurl4-openssl-dev \
        libvulkan1 \
        mesa-vulkan-drivers \
        ninja-build \
        rsync \
        vulkan-tools \
        vulkan-validationlayers \
    && rm -rf /var/lib/apt/lists/* \
    && ARCH="$(uname -m)" \
    && curl -fsSL "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${ARCH}.tar.gz" \
        | tar -xz --strip-components=1 -C /usr/local

# Same UTF-8 locale fix as the shared DevTools image — libarchive in
# CMake's FetchContent refuses non-ASCII filenames under C/POSIX.
ENV LANG=C.UTF-8 LC_ALL=C.UTF-8

# Runs the exact sequence from .github/workflows/build.yml for the
# Linux GCC matrix entry. Kept as a script so you can `docker run …
# ci-build` for a one-shot repro or invoke it manually inside an
# interactive shell.
RUN printf '%s\n' \
    '#!/usr/bin/env bash' \
    'set -euo pipefail' \
    'cd /workspace' \
    'cmake -G Ninja -B build-ci-linux -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ "$@"' \
    'cmake --build build-ci-linux' \
    'ctest --test-dir build-ci-linux --output-on-failure' \
    > /usr/local/bin/ci-build \
    && chmod +x /usr/local/bin/ci-build

WORKDIR /workspace

CMD ["/bin/bash"]
