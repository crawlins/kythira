# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The DynamoDB module's daemon in a container, for the client runs on Kythira
# (services/dynamodb/tools/evidence/clients_on_kythira.sh, tasks 11, 24): one
# `dynamodb_gateway_node` per container, addressed by compose service name.
#
# The build context is this directory. The binary is built on the host
# (Ubuntu 24.04, so this base carries the libstdc++ it was linked against)
# and copied into dist/ by clients_on_kythira.sh, never rebuilt here, as in
# services/kinesis/docker/kinesis_gateway_node/Dockerfile. No HEALTHCHECK:
# the script probes readiness itself, which also sidesteps Podman's
# OCI-format HEALTHCHECK drop.
#
# No capabilities, no privileged or host networking: CLAUDE.md's container
# rules require this to run under rootless Podman.

FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

# libatomic1: a clang build links some of core's consumers against
# libatomic, which the base image lacks.
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libatomic1 \
    && rm -rf /var/lib/apt/lists/*

COPY dist/dynamodb_gateway_node /usr/local/bin/dynamodb_gateway_node

# Fail the image build, naming the library, rather than leave a container
# to exit 127 with nothing in its log if the host build links a shared
# library this image does not carry.
RUN missing="$(ldd /usr/local/bin/dynamodb_gateway_node | sed -n '/not found/p')"; \
    if [ -n "$missing" ]; then echo "$missing" >&2; exit 1; fi

# The DynamoDB API and Raft RPC. See `dynamodb_gateway_node --help`.
EXPOSE 8000 7000

ENTRYPOINT ["/usr/local/bin/dynamodb_gateway_node"]
