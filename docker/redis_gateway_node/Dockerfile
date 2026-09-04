# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The Redis-compatible gateway host (.kiro/specs/redis-compatible-kv/ tasks
# 12-13): one `redis_gateway_node` per container, N containers, addressed by
# compose service name.
#
# Ubuntu 24.04 because the binary is built on the host against that libstdc++
# and COPIED in, never rebuilt here — the same doctrine as
# docker/multi_raft_node/Dockerfile. There is no HEALTHCHECK on purpose: the
# gateway speaks RESP, not HTTP, and PING answers -NOAUTH before AUTH, so a
# meaningful readiness probe needs a credential. The scenario tests probe
# readiness themselves (AUTH + PING + INFO on every node) through the
# published ports, which also sidesteps Podman's OCI-format HEALTHCHECK drop
# that scripts/build-tier-e-image.sh has to work around.
#
# No NET_ADMIN, no privileged networking, no host networking: CLAUDE.md's
# container rules require this to run under rootless Podman.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY docker/redis_gateway_node/dist/redis_gateway_node /usr/local/bin/redis_gateway_node

# Raft RPC and RESP. Configuration is entirely by environment; see
# `redis_gateway_node --help` or doc/redis-gateway.md.
EXPOSE 7000 6379

ENTRYPOINT ["/usr/local/bin/redis_gateway_node"]
