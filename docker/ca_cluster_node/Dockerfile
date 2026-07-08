# Stage 1 — build ca_cluster_node
FROM ubuntu:24.04 AS builder

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
        g++ cmake ninja-build pkg-config libssl-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH=/src/vcpkg_installed/x64-linux && \
    cmake --build build --target ca_cluster_node

# Stage 2 — runtime
FROM ubuntu:24.04

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
        libssl3 curl && \
    rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/ca_cluster_node /usr/local/bin/ca_cluster_node

# --data-dir defaults to /var/lib/ca_cluster_node (see cmd/ca_cluster_node/config.hpp)
# — mount a persistent volume (EFS on ECS, an EBS-backed host path for the
# manual EC2 path) there so file_persistence survives task/instance restarts,
# per Requirement 17's whole point of not losing state on restart.
VOLUME ["/var/lib/ca_cluster_node"]

ENTRYPOINT ["/usr/local/bin/ca_cluster_node"]
