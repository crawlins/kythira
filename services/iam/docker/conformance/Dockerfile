# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The IAM conformance stack's one image (.kiro/specs/iam-compatible-auth/
# task 23, services/iam/tests/docker/conformance.yml): iam_node,
# kythira-iam and iam_test_certs as in ../iam_node/Dockerfile, plus
# ca_cluster_node (the Kythira CA, the OIDC issuer under test) and
# redis_gateway_node built with CONFIG_REDIS_GATEWAY_IAM. Each compose
# service picks its binary with `entrypoint`, so one staged image serves the
# whole stack. A test image only: production images ship one daemon each.
#
# Same base and rules as ../iam_node/Dockerfile: binaries built on the host
# and copied in, no HEALTHCHECK, no capabilities, rootless Podman safe.

FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libatomic1 \
    && rm -rf /var/lib/apt/lists/*

COPY services/iam/docker/conformance/dist/iam_node \
     services/iam/docker/conformance/dist/kythira-iam \
     services/iam/docker/conformance/dist/iam_test_certs \
     services/iam/docker/conformance/dist/ca_cluster_node \
     services/iam/docker/conformance/dist/redis_gateway_node \
     /usr/local/bin/

RUN missing="$(for b in /usr/local/bin/iam_node /usr/local/bin/kythira-iam \
                         /usr/local/bin/iam_test_certs /usr/local/bin/ca_cluster_node \
                         /usr/local/bin/redis_gateway_node; do \
                 ldd "$b" | sed -n "s|^|$b: |; /not found/p"; done)"; \
    if [ -n "$missing" ]; then echo "$missing" >&2; exit 1; fi

ENTRYPOINT ["/usr/local/bin/iam_node"]
