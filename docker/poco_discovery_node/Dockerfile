FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libavahi-client3 \
        curl \
    && rm -rf /var/lib/apt/lists/*

COPY docker/poco_discovery_node/dist/poco_discovery_node /usr/local/bin/poco_discovery_node
COPY docker/poco_discovery_node/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

HEALTHCHECK --interval=2s --timeout=3s --start-period=15s --retries=20 \
    CMD curl -sf "http://localhost:${HTTP_PORT:-9011}/health" || exit 1

ENTRYPOINT ["/entrypoint.sh"]
