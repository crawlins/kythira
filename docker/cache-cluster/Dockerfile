# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The disk-backed build cache node (.kiro/specs/disk-backed-build-cache/
# task 8, Requirement 11): one `redis_gateway_node` in its default
# `KYTHIRA_CACHE_PERSISTENCE=file` mode per container, serving RESP and the
# HTTP/WebDAV front end over a chunk store on the `/data` volume.
#
# The binary is built on the host against Ubuntu 24.04's libstdc++ and
# COPIED in, never rebuilt here, as docker/redis_gateway_node/Dockerfile
# does. The image holds the daemon and its runtime libraries only.
#
# It runs as uid 10001, not root (Requirement 11.1). `/data` is created and
# handed to that user before the VOLUME line, so a fresh named volume starts
# out owned by it under Docker and under rootless Podman alike (both copy
# the image's directory ownership into a new named volume). No HEALTHCHECK,
# for the gateway image's reason: a meaningful probe needs a credential, and
# Podman's OCI image format would drop it anyway. The scenario tests probe
# readiness through the published ports.
#
# No capabilities, no privileged or host networking (Requirement 11.3).

FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --user-group --no-create-home \
        --home-dir /data --shell /usr/sbin/nologin kythira \
    && mkdir -p /data \
    && chown 10001:10001 /data \
    && chmod 0750 /data

COPY docker/cache-cluster/dist/redis_gateway_node /usr/local/bin/redis_gateway_node

ENV KYTHIRA_CACHE_PERSISTENCE=file \
    KYTHIRA_CACHE_DATA_DIR=/data

VOLUME /data
USER 10001:10001

# Raft RPC, RESP and the cache's HTTP front end. Configuration is entirely
# by environment; see `redis_gateway_node --help`.
EXPOSE 7000 6379 8080

ENTRYPOINT ["/usr/local/bin/redis_gateway_node"]
