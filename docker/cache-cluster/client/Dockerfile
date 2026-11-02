# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The CI-job side of the disk-backed build cache's end-to-end test
# (.kiro/specs/disk-backed-build-cache/ task 9, Requirement 12): the two
# real clients, unmodified, pointed at the cluster the way their own
# documentation says.
#
#   * sccache 0.17.0, the release tarball CI installs, checked against the
#     same SHA-256 sums .github/actions/oci-build-cache/action.yml pins. It
#     talks to the cluster through its WebDAV backend.
#   * vcpkg at vcpkg.json's builtin-baseline (the commit CI pins), with its
#     `http` binary source.
#
# CI's own sccache gate, scripts/oci-build-cache/start-sccache.sh, is copied
# in and used as-is, so "a build passes with every node stopped" is shown
# for the script CI actually runs, not for a test-only copy of its logic.
#
# Everything that needs the internet happens here, at image build time: the
# sccache download, the vcpkg checkout and tool bootstrap, and the zlib
# source tarball (`--only-downloads`). VCPKG_FORCE_SYSTEM_BINARIES keeps
# vcpkg on the system cmake and ninja. So when the test runs, the only
# network the client needs is the compose network, and stopping the cache
# nodes really does leave it with nothing to talk to.
#
# The image runs as root. It is the client, not the server: nothing in it
# listens, and it writes only inside its own throwaway container.

FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

ARG SCCACHE_VERSION=0.17.0
ARG SCCACHE_SHA256_X86_64=67c4a96dd237c1f518f6b36083f270f9976d516f1e57fce891755ea782e50006
ARG SCCACHE_SHA256_AARCH64=821a86343191aa1cbab74bd42f9e93c9a63bf85e4742945f40d3ae84193c1c77
ARG VCPKG_COMMIT=9a7f7340a6c5f11f24c3d59f85e07143feb84e06

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ca-certificates cmake curl git ninja-build \
        pkg-config tar unzip zip \
    && rm -rf /var/lib/apt/lists/*

RUN arch="$(uname -m)" \
    && case "$arch" in \
         x86_64)  sum="$SCCACHE_SHA256_X86_64" ;; \
         aarch64) sum="$SCCACHE_SHA256_AARCH64" ;; \
         *) echo "no pinned sccache checksum for $arch" >&2; exit 1 ;; \
       esac \
    && asset="sccache-v${SCCACHE_VERSION}-${arch}-unknown-linux-musl" \
    && curl --retry 3 -fsSL -o /tmp/sccache.tar.gz \
         "https://github.com/mozilla/sccache/releases/download/v${SCCACHE_VERSION}/${asset}.tar.gz" \
    && echo "${sum}  /tmp/sccache.tar.gz" | sha256sum -c - \
    && tar -xzf /tmp/sccache.tar.gz -C /usr/local/bin --strip-components=1 "${asset}/sccache" \
    && rm /tmp/sccache.tar.gz \
    && chmod 0755 /usr/local/bin/sccache \
    && sccache --version

# A shallow fetch of one commit: the whole vcpkg history is over 1 GB.
ENV VCPKG_ROOT=/opt/vcpkg \
    VCPKG_DISABLE_METRICS=1 \
    VCPKG_FORCE_SYSTEM_BINARIES=1
RUN git init -q "$VCPKG_ROOT" \
    && git -C "$VCPKG_ROOT" fetch -q --depth 1 https://github.com/microsoft/vcpkg.git "$VCPKG_COMMIT" \
    && git -C "$VCPKG_ROOT" checkout -q FETCH_HEAD \
    && "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics \
    && VCPKG_BINARY_SOURCES=clear "$VCPKG_ROOT/vcpkg" install zlib --only-downloads \
    && rm -rf "$VCPKG_ROOT/buildtrees" "$VCPKG_ROOT/packages" "$VCPKG_ROOT/installed"

COPY scripts/oci-build-cache/start-sccache.sh /usr/local/bin/start-sccache.sh
COPY docker/cache-cluster/client/run.sh /usr/local/bin/run.sh
COPY docker/cache-cluster/client/project /work/project
RUN chmod 0755 /usr/local/bin/start-sccache.sh /usr/local/bin/run.sh

WORKDIR /work
ENTRYPOINT ["/usr/local/bin/run.sh"]
