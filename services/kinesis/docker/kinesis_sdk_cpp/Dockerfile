# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The aws-sdk-cpp client of the Kinesis container scenario
# (.kiro/specs/kinesis-compatible-streams/ task 7, Requirement 9.4):
# kinesis_sdk_cpp.cpp (beside this file) built against the C++
# SDK's kinesis component only. The SDK is built here, not taken from the
# host's vcpkg tree, so the host build needs no kinesis feature and this
# image carries the one SDK version below. Built by the
# docker-kinesis-node-image target with the source root as its context.

FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55 AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        cmake \
        g++ \
        git \
        libcurl4-openssl-dev \
        libssl-dev \
        ninja-build \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# The SDK version the repository's other SDK builds use.
ARG AWS_SDK_CPP_VERSION=1.11.665
RUN git clone --depth 1 --branch "${AWS_SDK_CPP_VERSION}" --recurse-submodules \
        --shallow-submodules https://github.com/aws/aws-sdk-cpp.git /src/aws-sdk-cpp \
    && cmake -S /src/aws-sdk-cpp -B /build/aws-sdk-cpp -G Ninja \
        -DBUILD_ONLY=kinesis \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_TESTING=OFF \
        -DAUTORUN_UNIT_TESTS=OFF \
        -DCMAKE_INSTALL_PREFIX=/opt/aws-sdk-cpp \
    && cmake --build /build/aws-sdk-cpp \
    && cmake --install /build/aws-sdk-cpp \
    && rm -rf /src /build

COPY services/kinesis/docker/kinesis_sdk_cpp/ /src/kinesis_sdk_cpp/
RUN cmake -S /src/kinesis_sdk_cpp -B /build/kinesis_sdk_cpp -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/aws-sdk-cpp \
    && cmake --build /build/kinesis_sdk_cpp \
    && install -m 0755 /build/kinesis_sdk_cpp/kinesis_sdk_cpp /usr/local/bin/kinesis_sdk_cpp

FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libcurl4t64 \
        libssl3t64 \
        zlib1g \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local/bin/kinesis_sdk_cpp /usr/local/bin/kinesis_sdk_cpp

ENTRYPOINT ["/usr/local/bin/kinesis_sdk_cpp"]
