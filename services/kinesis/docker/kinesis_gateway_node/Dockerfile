# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The Kinesis module's daemon (.kiro/specs/kinesis-compatible-streams/
# task 7): one `kinesis_gateway_node` per container, addressed by compose
# service name.
#
# Ubuntu 24.04 because the binary is built on the host against that
# libstdc++ and COPIED in, never rebuilt here, as in
# services/iam/docker/iam_node/Dockerfile. No HEALTHCHECK: the scenario
# probes readiness itself through the AWS CLI, which also sidesteps
# Podman's OCI-format HEALTHCHECK drop.
#
# No capabilities, no privileged or host networking: CLAUDE.md's container
# rules require this to run under rootless Podman.

FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

# libatomic1: a clang build links network_simulator's consumers against
# libatomic, which the base image lacks.
# libnghttp2-14: HTTP/2 for SubscribeToShard (task 13), in case the host
# build links nghttp2 shared (vcpkg's default triplet links it statically).
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libatomic1 \
        libnghttp2-14 \
    && rm -rf /var/lib/apt/lists/*

# Everything staged in dist/: kinesis_gateway_node, chaos_s3_mock when the
# build has it (the scenario's tiering case runs it as the `s3mock` service
# from this same image, so no second image is needed), and
# dynamodb_gateway_node when the build has the DynamoDB module (the lease
# backend case's `ddb-leases`).
COPY services/kinesis/docker/kinesis_gateway_node/dist/ /usr/local/bin/

# Fail the image build, naming the library, rather than leave a container
# to exit 127 with nothing in its log if the host build links a shared
# library this image does not carry.
RUN test -x /usr/local/bin/kinesis_gateway_node; \
    for bin in /usr/local/bin/kinesis_gateway_node /usr/local/bin/chaos_s3_mock \
               /usr/local/bin/dynamodb_gateway_node; do \
      [ -e "$bin" ] || continue; \
      missing="$(ldd "$bin" | sed -n '/not found/p')"; \
      if [ -n "$missing" ]; then echo "$bin: $missing" >&2; exit 1; fi; \
    done

# Raft RPC and the Kinesis API. See `kinesis_gateway_node --help`.
EXPOSE 7100 4567

ENTRYPOINT ["/usr/local/bin/kinesis_gateway_node"]
