# ── Build stage ───────────────────────────────────────────────────────────────
# Uses pre-built vcpkg static libs (copied with COPY . .) so no vcpkg bootstrap
# is needed — only avahi dev headers must come from apt.
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake ninja-build g++ pkg-config \
        libavahi-client-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# CMAKE_PREFIX_PATH makes find_package() resolve from the vcpkg installed tree.
# uname -m-derived triplet so the same Dockerfile builds correctly whether
# the (native, non-emulated) build host is x86_64 or arm64.
RUN case "$(uname -m)" in \
        aarch64) TRIPLET=arm64-linux ;; \
        *)       TRIPLET=x64-linux ;; \
    esac; \
    cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_STANDARD=23 \
        -DCMAKE_PREFIX_PATH=/src/vcpkg_installed/$TRIPLET \
    && cmake --build build --target poco_discovery_node -j$(nproc)

# ── Runtime stage ─────────────────────────────────────────────────────────────
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libavahi-client3 \
        curl \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/poco_discovery_node /usr/local/bin/poco_discovery_node
COPY docker/poco_discovery_node/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

HEALTHCHECK --interval=2s --timeout=3s --start-period=15s --retries=20 \
    CMD curl -sf "http://localhost:${HTTP_PORT:-9011}/health" || exit 1

ENTRYPOINT ["/entrypoint.sh"]
