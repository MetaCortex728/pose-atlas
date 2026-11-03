# Development image for pose-atlas: the Ubuntu 22.04 toolchain used in CI.
FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        clang \
        cmake \
        g++ \
        gcc \
        git \
        ninja-build \
        python3 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
