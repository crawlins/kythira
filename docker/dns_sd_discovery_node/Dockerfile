# Stage 1 — build dns_sd_discovery_node
FROM ubuntu:24.04 AS builder

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
        g++ cmake ninja-build pkg-config \
        libldns-dev libavahi-client-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH=/src/vcpkg_installed/x64-linux && \
    cmake --build build --target dns_sd_discovery_node

# Stage 2 — runtime
FROM ubuntu:24.04

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
        libldns3 curl && \
    rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/dns_sd_discovery_node /usr/local/bin/dns_sd_discovery_node

HEALTHCHECK --interval=2s --timeout=3s --retries=30 \
    CMD curl -sf http://localhost:${HTTP_PORT:-9031}/health || exit 1

ENTRYPOINT ["/usr/local/bin/dns_sd_discovery_node"]
