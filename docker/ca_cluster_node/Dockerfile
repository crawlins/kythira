# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Stage 1 — build ca_cluster_node
FROM ubuntu:24.04 AS builder

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
        g++ cmake ninja-build pkg-config libssl-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# uname -m-derived triplet so the same Dockerfile builds correctly whether
# the (native, non-emulated) build host is x86_64 or arm64.
RUN case "$(uname -m)" in \
        aarch64) TRIPLET=arm64-linux ;; \
        *)       TRIPLET=x64-linux ;; \
    esac; \
    cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH=/src/vcpkg_installed/$TRIPLET && \
    cmake --build build --target ca_cluster_node

# Stage 2 — runtime
FROM ubuntu:24.04

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
        libssl3 curl && \
    rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/ca_cluster_node /usr/local/bin/ca_cluster_node

# Run as an unprivileged system user, the same `ca-cluster-node` account
# ca_cluster_node.service and the Packer AMI (packer/ca_cluster_node/) run it
# under on a host. A fixed numeric UID/GID, and a numeric USER, so a runtime
# that enforces "non-root" from the image config (Kubernetes runAsNonRoot)
# can verify it, and so persistent storage can be provisioned for it in
# advance: an EFS access point or a bind-mounted host directory must be owned
# by 10002:10002 (see ecs-task-definitions/README.md). It differs from
# ca_service's 10001 so the two never share file ownership on one host.
#
# The directories it may write are created here, owned by that user, because
# Docker and Podman both seed a new, empty named volume with the ownership of
# the image directory it is mounted on — and Fargate does the same for a
# task-storage volume mounted on a VOLUME path:
#   /var/lib/ca_cluster_node  --data-dir's default (cmd/ca_cluster_node/config.hpp).
#                             Mount a persistent volume (EFS on ECS, an
#                             EBS-backed host path for the manual EC2 path)
#                             there so file_persistence survives task/instance
#                             restarts, per Requirement 17's whole point of not
#                             losing state on restart.
#   /run/ca_cluster_node      where the ECS task definitions write the unseal
#                             key and RPC bootstrap credential from Secrets
#                             Manager, on task-local storage that never touches
#                             EFS; the root filesystem there is read-only.
RUN groupadd --system --gid 10002 ca-cluster-node && \
    useradd --system --uid 10002 --gid ca-cluster-node --no-create-home \
        --home-dir /nonexistent --shell /usr/sbin/nologin ca-cluster-node && \
    install -d -m 0750 -o ca-cluster-node -g ca-cluster-node /var/lib/ca_cluster_node && \
    install -d -m 0700 -o ca-cluster-node -g ca-cluster-node /run/ca_cluster_node
VOLUME ["/var/lib/ca_cluster_node", "/run/ca_cluster_node"]
USER 10002:10002

ENTRYPOINT ["/usr/local/bin/ca_cluster_node"]
