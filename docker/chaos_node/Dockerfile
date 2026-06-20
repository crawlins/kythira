# ── Build stage ──────────────────────────────────────────────────────────────
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake ninja-build clang-18 pkg-config git curl zip unzip tar \
        libfiu-dev \
        libboost-all-dev \
        nlohmann-json3-dev \
        libssl-dev \
    && rm -rf /var/lib/apt/lists/*

# Use clang-18 as the default compiler
ENV CC=clang-18 CXX=clang++-18

WORKDIR /src
COPY . .

# Build only the chaos_node target
RUN cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_CXX_STANDARD=23 \
        -DKYTHIRA_FAULT_INJECTION=ON \
    && cmake --build build --target chaos_node -j$(nproc)

# ── Runtime stage ─────────────────────────────────────────────────────────────
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libfiu0 fiu-utils \
        iproute2 \
        iptables \
        curl \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/chaos_node /usr/local/bin/chaos_node
COPY docker/chaos_node/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

EXPOSE 7000 8080 9000

HEALTHCHECK --interval=2s --timeout=3s --start-period=5s --retries=10 \
    CMD curl -sf "http://localhost:${HTTP_PORT:-8080}/health" || exit 1

ENTRYPOINT ["/entrypoint.sh"]
