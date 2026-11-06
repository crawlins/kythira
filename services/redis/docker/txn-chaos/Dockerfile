# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The transaction chaos node (.kiro/specs/multi-raft-transactions/ task 15):
# `redis_gateway_node` built with CONFIG_REDIS_GATEWAY_SNAPSHOT_ISOLATION
# (the `redis_gateway_node_snapshot` CMake target), so the KV is
# multi-versioned and every node holds a replica of the timestamp oracle.
#
# Built like services/redis/docker/redis_gateway_node/Dockerfile: the binary is compiled on
# the host against Ubuntu 24.04's libstdc++ and COPIED in. No HEALTHCHECK
# (a probe needs a credential; the scenario test probes the published
# ports), no capabilities, no privileged or host networking, so it runs
# under rootless Podman as under Docker (CLAUDE.md).

FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --user-group --no-create-home \
        --home-dir /nonexistent --shell /usr/sbin/nologin kythira

COPY services/redis/docker/txn-chaos/dist/redis_gateway_node /usr/local/bin/redis_gateway_node

USER 10001:10001

# Raft RPC and RESP. Configuration is entirely by environment.
EXPOSE 7000 6379

ENTRYPOINT ["/usr/local/bin/redis_gateway_node"]
