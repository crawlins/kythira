# Stage 1 — build ca_service
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
    cmake --build build --target ca_service

# Stage 2 — runtime
FROM ubuntu:24.04

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
        libssl3 curl && \
    rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/ca_service /usr/local/bin/ca_service

# No HEALTHCHECK for the default (oneshot) entrypoint: ca_service in
# provisioning mode runs to completion and exits, so compose gates on
# `service_completed_successfully`, not a health probe. The long-running
# `--serve` mode (docker/ca-service-server-compose.yml) overrides this
# Dockerfile's default command with its own HEALTHCHECK.
ENTRYPOINT ["/usr/local/bin/ca_service"]
