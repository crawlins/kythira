# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The engine chaos node (.kiro/specs/persistent-state-machine/ task 8,
# .kiro/specs/file-install-snapshot/ task 8): `redis_gateway_node` built
# with CONFIG_REDIS_GATEWAY_ENGINE=y, so each shard runs on
# persistent_kv_state_machine over one RocksDB database per node under
# /data.
#
# Built like services/redis/docker/cache-cluster/Dockerfile: the binary is
# compiled on the host against Ubuntu 24.04's libstdc++ and COPIED in
# (RocksDB is linked statically from vcpkg). It runs as uid 10001 with a
# /data volume; no HEALTHCHECK (a probe needs a credential; the scenario
# test probes the published ports), no capabilities, no privileged or host
# networking, so it runs under rootless Podman as under Docker (CLAUDE.md).

FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libatomic1 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --user-group --no-create-home \
        --home-dir /data --shell /usr/sbin/nologin kythira \
    && mkdir -p /data \
    && chown 10001:10001 /data \
    && chmod 0750 /data

COPY services/redis/docker/engine-chaos/dist/redis_gateway_node /usr/local/bin/redis_gateway_node

ENV KYTHIRA_CACHE_PERSISTENCE=engine \
    KYTHIRA_CACHE_DATA_DIR=/data

VOLUME /data
USER 10001:10001

# Raft RPC and RESP. Configuration is entirely by environment.
EXPOSE 7000 6379

ENTRYPOINT ["/usr/local/bin/redis_gateway_node"]
