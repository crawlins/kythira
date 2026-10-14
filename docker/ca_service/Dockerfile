# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Stage 1 — build ca_service
FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55 AS builder

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
    cmake --build build --target ca_service

# Stage 2 — runtime
FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
        libssl3 curl && \
    rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/ca_service /usr/local/bin/ca_service

# Run as an unprivileged system user, the same `ca-service` account
# docker/ca_service/ca_service.service runs under on a host. A fixed numeric
# UID/GID, and a numeric USER, so a runtime that enforces "non-root" from the
# image config (Kubernetes runAsNonRoot) can verify it without reading
# /etc/passwd, and so a bind-mounted host directory can be chowned to it in
# advance. 10001 sits above Ubuntu's 1000-range login users and inside the
# default 65536-entry subordinate UID range rootless Podman maps.
#
# The directories it may write are created here, owned by that user, because
# Docker and Podman both seed a new, empty named volume with the ownership of
# the image directory it is mounted on — and Fargate does the same for a
# task-storage volume mounted on a VOLUME path. Without them every volume
# would arrive owned by root and the service could not write it:
#   /ca              the oneshot --out-dir docker/ca-provisioning-compose.yml
#                    passes and mounts its shared volume on (a bind-mounted
#                    host --out-dir must be owned by 10001:10001 instead)
#   /run/ca_service  scratch space (TMPDIR) for the ECS task definition,
#                    whose root filesystem is read-only
RUN groupadd --system --gid 10001 ca-service && \
    useradd --system --uid 10001 --gid ca-service --no-create-home \
        --home-dir /nonexistent --shell /usr/sbin/nologin ca-service && \
    install -d -m 0755 -o ca-service -g ca-service /ca && \
    install -d -m 0700 -o ca-service -g ca-service /run/ca_service
VOLUME ["/run/ca_service"]
USER 10001:10001

# No HEALTHCHECK for the default (oneshot) entrypoint: ca_service in
# provisioning mode runs to completion and exits, so compose gates on
# `service_completed_successfully`, not a health probe. The long-running
# `--serve` mode (docker/ca-service-server-compose.yml) overrides this
# Dockerfile's default command with its own HEALTHCHECK.
ENTRYPOINT ["/usr/local/bin/ca_service"]
