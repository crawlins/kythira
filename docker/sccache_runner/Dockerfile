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
# The crate under test has no dependencies, so `cargo build` never touches
# crates.io — the only network the test needs is the compose network.

FROM docker.io/library/rust:1.85-slim-bookworm

ARG SCCACHE_VERSION=0.10.0

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && arch="$(uname -m)" \
    && curl -fsSL "https://github.com/mozilla/sccache/releases/download/v${SCCACHE_VERSION}/sccache-v${SCCACHE_VERSION}-${arch}-unknown-linux-musl.tar.gz" \
       | tar -xz -C /usr/local/bin --strip-components=1 "sccache-v${SCCACHE_VERSION}-${arch}-unknown-linux-musl/sccache" \
    && chmod +x /usr/local/bin/sccache \
    && sccache --version

COPY docker/sccache_runner/crate /work/crate
COPY docker/sccache_runner/run.sh /usr/local/bin/run.sh
RUN chmod +x /usr/local/bin/run.sh

WORKDIR /work
ENTRYPOINT ["/usr/local/bin/run.sh"]
