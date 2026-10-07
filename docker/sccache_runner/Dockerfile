# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# A real `sccache` in front of a real `rustc`, for the acceptance tests of
# .kiro/specs/redis-compatible-kv/ (Requirement 17). Nothing from this
# project is in the image: the point of the test is that an unmodified
# upstream client, configured the way its own documentation says, gets cache
# hits from the gateway.
#
# sccache is the upstream release tarball, not `cargo install sccache`, which
# would compile it (and its ~400 crates) on every image build. The version is
# pinned because the redis backend's environment variables changed shape at
# 0.5 (`SCCACHE_REDIS` URL → `SCCACHE_REDIS_ENDPOINT` + `_USERNAME` +
# `_PASSWORD`) and the runner script below uses the current shape.
#
# The tarball is checked against the SHA-256 that upstream publishes beside
# each release asset (`<asset>.sha256`), copied here so a replaced asset or a
# tampered download fails the build instead of running. Bumping
# SCCACHE_VERSION means updating both sums.
#
# The crate under test has no dependencies, so `cargo build` never touches
# crates.io — the only network the test needs is the compose network.

FROM docker.io/library/rust:1.85-slim-bookworm@sha256:9f841bbe9e7d8e37ceb96ed907265a3a0df7f44e3737d0b100e7907a679acb36

ARG SCCACHE_VERSION=0.10.0
ARG SCCACHE_SHA256_X86_64=1fbb35e135660d04a2d5e42b59c7874d39b3deb17de56330b25b713ec59f849b
ARG SCCACHE_SHA256_AARCH64=d6a1ce4acd02b937cd61bc675a8be029a60f7bc167594c33d75732bbc0a07400

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && arch="$(uname -m)" \
    && case "$arch" in \
         x86_64)  sum="$SCCACHE_SHA256_X86_64" ;; \
         aarch64) sum="$SCCACHE_SHA256_AARCH64" ;; \
         *) echo "no pinned sccache checksum for $arch" >&2; exit 1 ;; \
       esac \
    && asset="sccache-v${SCCACHE_VERSION}-${arch}-unknown-linux-musl" \
    && curl -fsSL -o /tmp/sccache.tar.gz \
         "https://github.com/mozilla/sccache/releases/download/v${SCCACHE_VERSION}/${asset}.tar.gz" \
    && echo "${sum}  /tmp/sccache.tar.gz" | sha256sum -c - \
    && tar -xzf /tmp/sccache.tar.gz -C /usr/local/bin --strip-components=1 "${asset}/sccache" \
    && rm /tmp/sccache.tar.gz \
    && chmod +x /usr/local/bin/sccache \
    && sccache --version

COPY docker/sccache_runner/crate /work/crate
COPY docker/sccache_runner/run.sh /usr/local/bin/run.sh
RUN chmod +x /usr/local/bin/run.sh

WORKDIR /work
ENTRYPOINT ["/usr/local/bin/run.sh"]
