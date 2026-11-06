# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The IAM module's daemon (.kiro/specs/iam-compatible-auth/ task 9): one
# `iam_node` per container, addressed by compose service name, with the
# `kythira-iam` admin CLI beside it so a scenario can drive the cluster with
# `compose exec`.
#
# The image also carries `iam_test_certs`, which writes the throwaway test
# PKI of services/iam/tests/iam_test_pki.hpp. It is what the compose
# scenario's one-shot `certs` service runs; nothing else uses it, and a
# production image would ship only the first two binaries.
#
# Ubuntu 24.04 because the binaries are built on the host against that
# libstdc++ and COPIED in, never rebuilt here, as in
# services/redis/docker/redis_gateway_node/Dockerfile. No HEALTHCHECK: the
# scenario probes readiness itself with `kythira-iam health`, which also
# sidesteps Podman's OCI-format HEALTHCHECK drop.
#
# No capabilities, no privileged or host networking: CLAUDE.md's container
# rules require this to run under rootless Podman.

FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

# libatomic1: a clang build links network_simulator's consumers against
# libatomic (see the Azure SDK block in CMakeLists.txt), and the base image
# lacks it, so iam_node and iam_test_certs exited 127 at load time.
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        libatomic1 \
    && rm -rf /var/lib/apt/lists/*

COPY services/iam/docker/iam_node/dist/iam_node /usr/local/bin/iam_node
COPY services/iam/docker/iam_node/dist/kythira-iam /usr/local/bin/kythira-iam
COPY services/iam/docker/iam_node/dist/iam_test_certs /usr/local/bin/iam_test_certs

# Fail the image build, naming the library, rather than leave a container
# to exit 127 with nothing in its log if the host build links a shared
# library this image does not carry.
RUN missing="$(for b in /usr/local/bin/iam_node /usr/local/bin/kythira-iam \
                         /usr/local/bin/iam_test_certs; do \
                 ldd "$b" | sed -n "s|^|$b: |; /not found/p"; done)"; \
    if [ -n "$missing" ]; then echo "$missing" >&2; exit 1; fi

# Raft RPC (TLS over TCP) and the HTTPS API. See `iam_node --help`.
EXPOSE 7100 8443

ENTRYPOINT ["/usr/local/bin/iam_node"]
